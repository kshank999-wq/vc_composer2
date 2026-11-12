// A score as a standard MIDI file (spec 01 section 7, spec 02 section 6).
//
// This is the export path the PTSL client's import uses and the
// drag-and-drop fallback uses. Checked by parsing the written file back, so
// what is asserted is what a DAW would actually read - not what this code
// believes it wrote.
#include "vcc/notation/ScoreMidiFile.h"

#include <algorithm>
#include <string>
#include <vector>

#include "vcc_test.h"
#include "vcc_test_files.h"

using namespace vcc::notation;
using vcc::music::MidiEventType;

/// A quarter note in ticks, so the arithmetic reads as rhythm.

namespace {

ScoreNote make_note(Ticks tick, Step step, std::int8_t octave = 4,
                    Duration duration = quarter())
{
    ScoreNote note;
    note.start_tick = tick;
    note.duration = duration;
    note.pitch = Pitch{step, octave, 0};
    return note;
}

/// A short phrase: four quarter notes from the first beat.
Score four_quarters(const char* staff_name = "Strings")
{
    Score score;
    score.staff(0).name = staff_name;
    for (int i = 0; i < 4; ++i) {
        score.insert_note(0, make_note(i * kTicksPerQuarter,
                                       static_cast<Step>(static_cast<int>(Step::C) + i)));
    }
    return score;
}

// ---------------------------------------------------------------------------
// The same independent reader as test_midi_file, reduced to what is needed
// here: a written file is checked by parsing it, never by comparing against
// bytes produced by the same code.
// ---------------------------------------------------------------------------

struct ReadEvent {
    std::size_t track = 0;
    std::int64_t tick = 0;
    bool is_meta = false;
    std::uint8_t meta_type = 0;
    std::vector<std::uint8_t> data;

    std::string text() const { return std::string(data.begin(), data.end()); }
    std::uint8_t status_nibble() const
    {
        return data.empty() ? 0 : static_cast<std::uint8_t>(data[0] >> 4);
    }
    std::uint8_t channel() const
    {
        return data.empty() ? 0 : static_cast<std::uint8_t>(data[0] & 0x0F);
    }
};

struct ParsedSmf {
    bool ok = false;
    std::string error;
    int format = -1;
    int track_count = 0;
    int division = 0;
    std::vector<ReadEvent> events;

    std::vector<ReadEvent> meta_of_type(std::uint8_t type, int track = -1) const
    {
        std::vector<ReadEvent> found;
        for (const ReadEvent& event : events) {
            if (event.is_meta && event.meta_type == type
                && (track < 0 || event.track == static_cast<std::size_t>(track))) {
                found.push_back(event);
            }
        }
        return found;
    }

    std::vector<ReadEvent> note_ons(int track = -1) const
    {
        std::vector<ReadEvent> found;
        for (const ReadEvent& event : events) {
            if (!event.is_meta && event.status_nibble() == 0x09 && event.data[2] > 0
                && (track < 0 || event.track == static_cast<std::size_t>(track))) {
                found.push_back(event);
            }
        }
        return found;
    }
};

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t at)
{
    return (static_cast<std::uint32_t>(bytes[at]) << 24)
           | (static_cast<std::uint32_t>(bytes[at + 1]) << 16)
           | (static_cast<std::uint32_t>(bytes[at + 2]) << 8)
           | static_cast<std::uint32_t>(bytes[at + 3]);
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t at)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[at]) << 8)
                                      | bytes[at + 1]);
}

bool matches_id(const std::vector<std::uint8_t>& bytes, std::size_t at, const char* id)
{
    for (int i = 0; i < 4; ++i) {
        if (at + static_cast<std::size_t>(i) >= bytes.size()
            || bytes[at + static_cast<std::size_t>(i)] != static_cast<std::uint8_t>(id[i])) {
            return false;
        }
    }
    return true;
}

ParsedSmf parse_smf(const std::vector<std::uint8_t>& bytes)
{
    ParsedSmf file;
    if (bytes.size() < 14 || !matches_id(bytes, 0, "MThd") || read_u32(bytes, 4) != 6) {
        file.error = "not a MIDI file header";
        return file;
    }

    file.format = read_u16(bytes, 8);
    file.track_count = read_u16(bytes, 10);
    file.division = read_u16(bytes, 12);

    std::size_t at = 14;
    for (int track = 0; track < file.track_count; ++track) {
        if (at + 8 > bytes.size() || !matches_id(bytes, at, "MTrk")) {
            file.error = "no MTrk where one was expected";
            return file;
        }
        const std::size_t end = at + 8 + read_u32(bytes, at + 4);
        at += 8;
        if (end > bytes.size()) {
            file.error = "a track runs past the end of the file";
            return file;
        }

        std::int64_t tick = 0;
        while (at < end) {
            std::int64_t delta = 0;
            if (!vcc::music::read_variable_length(bytes.data(), end, at, delta)) {
                file.error = "a delta time is malformed";
                return file;
            }
            tick += delta;
            if (at >= end) {
                file.error = "a track ends mid-event";
                return file;
            }

            ReadEvent event;
            event.track = static_cast<std::size_t>(track);
            event.tick = tick;

            const std::uint8_t status = bytes[at++];
            if (status == 0xFF) {
                event.is_meta = true;
                event.meta_type = bytes[at++];
                std::int64_t payload = 0;
                if (!vcc::music::read_variable_length(bytes.data(), end, at, payload)
                    || at + static_cast<std::size_t>(payload) > end) {
                    file.error = "a meta event is malformed";
                    return file;
                }
                event.data.assign(
                    bytes.begin() + static_cast<std::ptrdiff_t>(at),
                    bytes.begin()
                        + static_cast<std::ptrdiff_t>(at + static_cast<std::size_t>(payload)));
                at += static_cast<std::size_t>(payload);
            } else if ((status & 0x80) == 0 || status >= 0xF0) {
                file.error = "running status or a system message";
                return file;
            } else {
                const std::uint8_t high = static_cast<std::uint8_t>(status >> 4);
                const std::size_t data_bytes = (high == 0x0C || high == 0x0D) ? 1u : 2u;
                if (at + data_bytes > end) {
                    file.error = "a channel message is truncated";
                    return file;
                }
                event.data.push_back(status);
                for (std::size_t i = 0; i < data_bytes; ++i)
                    event.data.push_back(bytes[at++]);
            }

            file.events.push_back(std::move(event));
        }
    }

    file.ok = at == bytes.size();
    if (!file.ok)
        file.error = "trailing bytes after the last track";
    return file;
}

/// Builds, writes, reads and parses - the whole path, which is the only way
/// to know the file a DAW gets is well formed.
ParsedSmf round_trip(const Score& score, const MidiRenderer& renderer,
                    const ScoreMidiOptions& options, ScoreMidiResult& built,
                    std::string& write_error)
{
    const vcc::test::TempFile file("score-midi", ".mid");
    const vcc::music::SmfWriteResult written =
        write_score_midi(score, renderer, file.path(), options, &built);
    write_error = written.error;
    if (!written.ok)
        return ParsedSmf{};

    std::vector<std::uint8_t> bytes;
    if (!vcc::test::read_file(file.path(), bytes)) {
        write_error = "nothing was written";
        return ParsedSmf{};
    }
    return parse_smf(bytes);
}

}  // namespace

// ---------------------------------------------------------------------------
// The shape of the file
// ---------------------------------------------------------------------------

VCC_TEST(a_score_exports_as_a_conductor_track_and_a_music_track)
{
    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read =
        round_trip(four_quarters(), MidiRenderer{}, ScoreMidiOptions{}, built, error);

    VCC_CHECK_MSG(read.ok, error + read.error);
    VCC_CHECK_EQ(read.format, 1);
    VCC_CHECK_EQ(read.track_count, 2);
    VCC_CHECK(built.has_conductor_track);
    VCC_CHECK_EQ(built.music_track_count, static_cast<std::size_t>(1));
    VCC_CHECK_EQ(built.note_count, static_cast<std::size_t>(4));
    VCC_CHECK(built.carried_everything());

    // The notation engine's resolution goes into the header unchanged, so no
    // position is ever rescaled.
    // The host's resolution, not the score's.
    VCC_CHECK_EQ(read.division, static_cast<int>(vcc::music::kTicksPerQuarter));
}

VCC_TEST(the_tempo_map_is_in_the_conductor_track_and_the_notes_are_not)
{
    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read =
        round_trip(four_quarters(), MidiRenderer{}, ScoreMidiOptions{}, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    // Track 0 carries the meta events and no notes; track 1 the reverse.
    VCC_CHECK(read.note_ons(0).empty());
    VCC_CHECK_EQ(read.note_ons(1).size(), static_cast<std::size_t>(4));

    VCC_CHECK_EQ(read.meta_of_type(vcc::music::smf_meta::kTempo, 0).size(),
                 static_cast<std::size_t>(1));
    VCC_CHECK_EQ(read.meta_of_type(vcc::music::smf_meta::kTimeSignature, 0).size(),
                 static_cast<std::size_t>(1));
    VCC_CHECK_EQ(read.meta_of_type(vcc::music::smf_meta::kKeySignature, 0).size(),
                 static_cast<std::size_t>(1));
    VCC_CHECK(read.meta_of_type(vcc::music::smf_meta::kTempo, 1).empty());
}

VCC_TEST(one_track_is_format_zero_which_every_reader_understands)
{
    ScoreMidiOptions options;
    options.write_conductor_track = false;

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(four_quarters(), MidiRenderer{}, options, built, error);

    VCC_CHECK_MSG(read.ok, error + read.error);
    VCC_CHECK_EQ(read.format, 0);
    VCC_CHECK_EQ(read.track_count, 1);
    VCC_CHECK(!built.has_conductor_track);
    VCC_CHECK_EQ(read.note_ons(0).size(), static_cast<std::size_t>(4));
}

VCC_TEST(a_grand_staff_exports_as_one_track_because_it_is_one_instrument)
{
    // A Score is the notation for one bound Pro Tools track (spec 02
    // section 4.2), and a piano's two hands are one part. Splitting them
    // would import as two tracks the composer then has to merge.
    Score score;
    score.make_grand_staff();
    score.staff(0).name = "Piano";
    score.insert_note(0, make_note(0, Step::C, 5));
    score.insert_note(1, make_note(0, Step::C, 3));

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, ScoreMidiOptions{}, built, error);

    VCC_CHECK_MSG(read.ok, error + read.error);
    VCC_CHECK_EQ(read.track_count, 2);  // Conductor plus one music track.
    VCC_CHECK_EQ(built.music_track_count, static_cast<std::size_t>(1));
    VCC_CHECK_EQ(read.note_ons(1).size(), static_cast<std::size_t>(2));

    // Both hands on one channel, since they are one instrument.
    for (const ReadEvent& note : read.note_ons(1))
        VCC_CHECK_EQ(note.channel(), static_cast<std::uint8_t>(0));
}

VCC_TEST(a_score_can_also_be_exported_with_one_track_per_staff)
{
    Score score;
    score.make_grand_staff();
    score.staff(0).name = "Right hand";
    score.staff(1).name = "Left hand";
    score.insert_note(0, make_note(0, Step::C, 5));
    score.insert_note(1, make_note(0, Step::C, 3));

    ScoreMidiOptions options;
    options.one_track_per_staff = true;
    options.first_channel = 2;

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, options, built, error);

    VCC_CHECK_MSG(read.ok, error + read.error);
    VCC_CHECK_EQ(read.track_count, 3);
    VCC_CHECK_EQ(built.music_track_count, static_cast<std::size_t>(2));

    VCC_CHECK_EQ(read.note_ons(1).size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(read.note_ons(2).size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(read.note_ons(1)[0].channel(), static_cast<std::uint8_t>(2));
    VCC_CHECK_EQ(read.note_ons(2)[0].channel(), static_cast<std::uint8_t>(3));

    // Each track carries its own staff's music: C5 above, C3 below. Counts
    // alone would be satisfied by rendering the same staff twice.
    VCC_CHECK_EQ(read.note_ons(1)[0].data[1], static_cast<std::uint8_t>(72));
    VCC_CHECK_EQ(read.note_ons(2)[0].data[1], static_cast<std::uint8_t>(48));

    const std::vector<ReadEvent> names =
        read.meta_of_type(vcc::music::smf_meta::kTrackName);
    VCC_CHECK_EQ(names.size(), static_cast<std::size_t>(3));
    VCC_CHECK_EQ(names[1].text(), std::string("Right hand"));
    VCC_CHECK_EQ(names[2].text(), std::string("Left hand"));
}

VCC_TEST(channels_wrap_rather_than_running_off_the_end_of_midi)
{
    // The behaviour, not the layer that implements it: MidiEvent masks a
    // channel to four bits as well, so a staff past the sixteenth lands on a
    // real channel either way rather than producing events a file cannot
    // carry.
    Score score;
    score.staff(0).name = "S1";
    score.insert_note(0, make_note(0, Step::C, 3));
    for (int i = 1; i < 4; ++i) {
        Staff staff;
        staff.name = "S" + std::to_string(i + 1);
        score.add_staff(staff);
        score.insert_note(static_cast<std::size_t>(i),
                          make_note(0, Step::C, static_cast<std::int8_t>(3 + i)));
    }

    ScoreMidiOptions options;
    options.one_track_per_staff = true;
    options.first_channel = 14;

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, options, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    const std::uint8_t expected_channel[] = {14, 15, 0, 1};
    const std::uint8_t expected_note[] = {48, 60, 72, 84};  // C3 to C6.
    for (std::size_t staff = 0; staff < 4; ++staff) {
        const std::vector<ReadEvent> notes = read.note_ons(static_cast<int>(staff) + 1);
        VCC_CHECK_EQ(notes.size(), static_cast<std::size_t>(1));
        VCC_CHECK_EQ(notes[0].channel(), expected_channel[staff]);
        VCC_CHECK_EQ(notes[0].data[1], expected_note[staff]);
    }
}

// ---------------------------------------------------------------------------
// Naming
// ---------------------------------------------------------------------------

VCC_TEST(a_track_always_has_a_name)
{
    // A DAW shows it as the imported track's name, and an unnamed track is
    // one the composer cannot find again.
    Score score;  // No staff name at all.
    score.insert_note(0, make_note(0, Step::C));

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, ScoreMidiOptions{}, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    const std::vector<ReadEvent> names =
        read.meta_of_type(vcc::music::smf_meta::kTrackName);
    VCC_CHECK_EQ(names.size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(names[0].text(), std::string("Conductor"));
    VCC_CHECK(!names[1].text().empty());
}

VCC_TEST(the_staff_name_becomes_the_track_name_and_an_explicit_one_wins)
{
    {
        ScoreMidiResult built;
        std::string error;
        const ParsedSmf read = round_trip(four_quarters("Violin I"), MidiRenderer{},
                                         ScoreMidiOptions{}, built, error);
        VCC_CHECK_MSG(read.ok, error + read.error);
        VCC_CHECK_EQ(read.meta_of_type(vcc::music::smf_meta::kTrackName, 1)[0].text(),
                     std::string("Violin I"));
    }
    {
        ScoreMidiOptions options;
        options.track_name = "Cue 12 strings";
        options.conductor_track_name = "Map";

        ScoreMidiResult built;
        std::string error;
        const ParsedSmf read =
            round_trip(four_quarters("Violin I"), MidiRenderer{}, options, built, error);
        VCC_CHECK_MSG(read.ok, error + read.error);
        VCC_CHECK_EQ(read.meta_of_type(vcc::music::smf_meta::kTrackName, 0)[0].text(),
                     std::string("Map"));
        VCC_CHECK_EQ(read.meta_of_type(vcc::music::smf_meta::kTrackName, 1)[0].text(),
                     std::string("Cue 12 strings"));
    }
}

// ---------------------------------------------------------------------------
// The tempo map
// ---------------------------------------------------------------------------

VCC_TEST(tempo_and_meter_changes_reach_the_conductor_track)
{
    Score score = four_quarters();
    VCC_CHECK(score.set_time_signature(0, vcc::music::TimeSignature{3, 4}));
    // Bar 3 of 3/4 starts at 6 quarter notes.
    VCC_CHECK(score.set_time_signature(6 * kTicksPerQuarter,
                                       vcc::music::TimeSignature{7, 8}));
    score.add_tempo(TempoMarking{0, 84.0, NoteValue::Quarter, 0, "Andante"});
    score.add_tempo(TempoMarking{4 * kTicksPerQuarter, 120.0, NoteValue::Quarter, 0, ""});
    VCC_CHECK(score.set_key_signature(0, KeySignature{-3, false}));
    VCC_CHECK(score.set_key_signature(3 * kTicksPerQuarter, KeySignature{2, true}));

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, ScoreMidiOptions{}, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);
    VCC_CHECK(built.unrepresentable.empty());

    const std::vector<ReadEvent> tempos = read.meta_of_type(vcc::music::smf_meta::kTempo, 0);
    VCC_CHECK_EQ(tempos.size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(tempos[0].tick, static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(tempos[1].tick, static_cast<std::int64_t>(to_host_ticks(4 * kTicksPerQuarter)));

    // 84 quarter notes per minute is 714286 microseconds each.
    const std::int64_t first = (static_cast<std::int64_t>(tempos[0].data[0]) << 16)
                               | (static_cast<std::int64_t>(tempos[0].data[1]) << 8)
                               | static_cast<std::int64_t>(tempos[0].data[2]);
    VCC_CHECK_EQ(first, static_cast<std::int64_t>(714286));

    const std::vector<ReadEvent> meters =
        read.meta_of_type(vcc::music::smf_meta::kTimeSignature, 0);
    VCC_CHECK_EQ(meters.size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(meters[0].data[0], static_cast<std::uint8_t>(3));
    VCC_CHECK_EQ(meters[1].data[0], static_cast<std::uint8_t>(7));
    VCC_CHECK_EQ(meters[1].data[1], static_cast<std::uint8_t>(3));  // Eighths.
    // At its tick, in the file's own: six quarters of 960.
    VCC_CHECK_EQ(meters[1].tick, static_cast<std::int64_t>(to_host_ticks(6 * kTicksPerQuarter)));

    const std::vector<ReadEvent> keys =
        read.meta_of_type(vcc::music::smf_meta::kKeySignature, 0);
    VCC_CHECK_EQ(keys.size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(static_cast<std::int8_t>(keys[0].data[0]), static_cast<std::int8_t>(-3));
    VCC_CHECK_EQ(keys[1].data[1], static_cast<std::uint8_t>(1));  // Minor.
}

VCC_TEST(a_change_at_the_start_of_the_range_is_not_written_twice)
{
    Score score = four_quarters();
    score.add_tempo(TempoMarking{0, 84.0, NoteValue::Quarter, 0, ""});
    VCC_CHECK(score.set_time_signature(0, vcc::music::TimeSignature{5, 8}));

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, ScoreMidiOptions{}, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    VCC_CHECK_EQ(read.meta_of_type(vcc::music::smf_meta::kTempo, 0).size(),
                 static_cast<std::size_t>(1));
    VCC_CHECK_EQ(read.meta_of_type(vcc::music::smf_meta::kTimeSignature, 0).size(),
                 static_cast<std::size_t>(1));
}

VCC_TEST(text_markings_become_markers_on_the_conductor_track)
{
    Score score = four_quarters();
    score.add_text(TextMarking{0, 0, "A"});
    score.add_text(TextMarking{2 * kTicksPerQuarter, 1, "Hit"});
    score.add_text(TextMarking{8 * kTicksPerQuarter, 0, "B"});

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, ScoreMidiOptions{}, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);
    VCC_CHECK_EQ(built.marker_count, static_cast<std::size_t>(3));

    // On the conductor track, at their ticks, as what was written - the
    // one at the first tick included, since nothing else writes it.
    const std::vector<ReadEvent> markers = read.meta_of_type(vcc::music::smf_meta::kMarker, 0);
    VCC_CHECK_EQ(markers.size(), static_cast<std::size_t>(3));
    if (markers.size() == 3) {
        VCC_CHECK_EQ(markers[0].tick, static_cast<std::int64_t>(0));
        VCC_CHECK_EQ(markers[0].text(), std::string("A"));
        VCC_CHECK_EQ(markers[1].tick, static_cast<std::int64_t>(to_host_ticks(2 * kTicksPerQuarter)));
        VCC_CHECK_EQ(markers[1].text(), std::string("Hit"));
        VCC_CHECK_EQ(markers[2].text(), std::string("B"));
    }
    VCC_CHECK(read.meta_of_type(vcc::music::smf_meta::kMarker, 1).empty());

    // Without a conductor track they go on the music track; turned off,
    // nowhere.
    ScoreMidiOptions no_conductor;
    no_conductor.write_conductor_track = false;
    const ParsedSmf one_track = round_trip(score, MidiRenderer{}, no_conductor, built, error);
    VCC_CHECK_MSG(one_track.ok, error + one_track.error);
    VCC_CHECK_EQ(one_track.meta_of_type(vcc::music::smf_meta::kMarker, 0).size(),
                 static_cast<std::size_t>(3));

    ScoreMidiOptions none;
    none.write_text_as_markers = false;
    const ParsedSmf without = round_trip(score, MidiRenderer{}, none, built, error);
    VCC_CHECK_MSG(without.ok, error + without.error);
    VCC_CHECK_EQ(built.marker_count, static_cast<std::size_t>(0));
    VCC_CHECK(without.meta_of_type(vcc::music::smf_meta::kMarker, 0).empty());
}

VCC_TEST(markers_follow_the_exported_range_and_its_rebasing)
{
    Score score = four_quarters();
    score.add_text(TextMarking{0, 0, "A"});
    score.add_text(TextMarking{2 * kTicksPerQuarter, 0, "Hit"});
    score.add_text(TextMarking{4 * kTicksPerQuarter, 0, "B"});

    // Bar 1 beat 3 to bar 2: the marker at the start of the range comes at
    // the file's tick 0, the one before it does not come, and the one at
    // the range's end is outside it.
    ScoreMidiOptions options;
    options.render.from_tick = 2 * kTicksPerQuarter;
    options.render.to_tick = 4 * kTicksPerQuarter;

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, options, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);
    const std::vector<ReadEvent> markers = read.meta_of_type(vcc::music::smf_meta::kMarker, 0);
    VCC_CHECK_EQ(markers.size(), static_cast<std::size_t>(1));
    if (!markers.empty()) {
        VCC_CHECK_EQ(markers[0].text(), std::string("Hit"));
        VCC_CHECK_EQ(markers[0].tick, static_cast<std::int64_t>(0));
    }

    options.rebase_to_range_start = false;
    const ParsedSmf absolute = round_trip(score, MidiRenderer{}, options, built, error);
    VCC_CHECK_MSG(absolute.ok, error + absolute.error);
    const std::vector<ReadEvent> kept = absolute.meta_of_type(vcc::music::smf_meta::kMarker, 0);
    VCC_CHECK_EQ(kept.size(), static_cast<std::size_t>(1));
    if (!kept.empty())
        VCC_CHECK_EQ(kept[0].tick, static_cast<std::int64_t>(to_host_ticks(2 * kTicksPerQuarter)));
}

VCC_TEST(a_score_with_no_tempo_marking_takes_the_option)
{
    ScoreMidiOptions options;
    options.default_tempo_bpm = 66.0;

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(four_quarters(), MidiRenderer{}, options, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    const std::vector<ReadEvent> tempos = read.meta_of_type(vcc::music::smf_meta::kTempo, 0);
    VCC_CHECK_EQ(tempos.size(), static_cast<std::size_t>(1));
    const std::int64_t microseconds = (static_cast<std::int64_t>(tempos[0].data[0]) << 16)
                                      | (static_cast<std::int64_t>(tempos[0].data[1]) << 8)
                                      | static_cast<std::int64_t>(tempos[0].data[2]);
    VCC_CHECK_EQ(microseconds, static_cast<std::int64_t>(909091));  // 60e6 / 66.
}

VCC_TEST(a_score_with_a_tempo_marking_ignores_the_option)
{
    // The score's own answer, which is not this writer's to second-guess.
    Score score = four_quarters();
    score.add_tempo(TempoMarking{0, 100.0, NoteValue::Quarter, 0, ""});

    ScoreMidiOptions options;
    options.default_tempo_bpm = 66.0;

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, options, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    const std::vector<ReadEvent> tempos = read.meta_of_type(vcc::music::smf_meta::kTempo, 0);
    VCC_CHECK_EQ(tempos.size(), static_cast<std::size_t>(1));
    const std::int64_t microseconds = (static_cast<std::int64_t>(tempos[0].data[0]) << 16)
                                      | (static_cast<std::int64_t>(tempos[0].data[1]) << 8)
                                      | static_cast<std::int64_t>(tempos[0].data[2]);
    VCC_CHECK_EQ(microseconds, static_cast<std::int64_t>(600000));
}

VCC_TEST(a_dotted_beat_unit_is_converted_rather_than_written_as_written)
{
    // "Dotted quarter = 60" in 6/8 is 90 quarter notes per minute, and the
    // file stores quarter notes. The score keeps what was written; the
    // export has to state what it means.
    Score score = four_quarters();
    score.add_tempo(TempoMarking{0, 60.0, NoteValue::Quarter, 1, "" });

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, ScoreMidiOptions{}, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    const std::vector<ReadEvent> tempos = read.meta_of_type(vcc::music::smf_meta::kTempo, 0);
    const std::int64_t microseconds = (static_cast<std::int64_t>(tempos[0].data[0]) << 16)
                                      | (static_cast<std::int64_t>(tempos[0].data[1]) << 8)
                                      | static_cast<std::int64_t>(tempos[0].data[2]);
    // 90 quarter notes per minute.
    VCC_CHECK_EQ(microseconds, static_cast<std::int64_t>(666667));
}

// ---------------------------------------------------------------------------
// Exporting part of a score
// ---------------------------------------------------------------------------

VCC_TEST(a_range_export_is_rebased_so_the_file_starts_at_what_was_exported)
{
    // What an import wants: the caller says where to place the file, and the
    // file begins at the passage, not after two bars of silence.
    Score score;
    score.staff(0).name = "Strings";
    for (int bar = 0; bar < 4; ++bar)
        score.insert_note(0, make_note(bar * 4 * kTicksPerQuarter, Step::C));

    ScoreMidiOptions options;
    options.render.from_tick = 2 * 4 * kTicksPerQuarter;  // Bar 3.
    options.render.to_tick = 4 * 4 * kTicksPerQuarter;

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, options, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    const std::vector<ReadEvent> notes = read.note_ons(1);
    VCC_CHECK_EQ(notes.size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(notes[0].tick, static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(notes[1].tick, static_cast<std::int64_t>(to_host_ticks(4 * kTicksPerQuarter)));
}

VCC_TEST(rebasing_can_be_turned_off_to_keep_absolute_positions)
{
    Score score;
    score.staff(0).name = "Strings";
    for (int bar = 0; bar < 4; ++bar)
        score.insert_note(0, make_note(bar * 4 * kTicksPerQuarter, Step::C));

    ScoreMidiOptions options;
    options.rebase_to_range_start = false;
    options.render.from_tick = 2 * 4 * kTicksPerQuarter;
    options.render.to_tick = 4 * 4 * kTicksPerQuarter;

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, options, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    const std::vector<ReadEvent> notes = read.note_ons(1);
    VCC_CHECK_EQ(notes.size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(notes[0].tick, static_cast<std::int64_t>(to_host_ticks(2 * 4 * kTicksPerQuarter)));
}

VCC_TEST(the_meter_in_force_at_the_range_start_is_written_not_the_first_one)
{
    // A passage exported from bar 9 of a piece that changes to 7/8 at bar 5
    // is in 7/8, and a file that said 4/4 would be read wrongly.
    Score score = four_quarters();
    VCC_CHECK(score.set_time_signature(0, vcc::music::TimeSignature{4, 4}));
    VCC_CHECK(score.set_time_signature(4 * 4 * kTicksPerQuarter,
                                       vcc::music::TimeSignature{7, 8}));
    score.add_tempo(TempoMarking{0, 60.0, NoteValue::Quarter, 0, ""});
    score.add_tempo(TempoMarking{4 * 4 * kTicksPerQuarter, 144.0, NoteValue::Quarter, 0, ""});

    ScoreMidiOptions options;
    options.render.from_tick = 6 * 4 * kTicksPerQuarter;
    options.render.to_tick = 8 * 4 * kTicksPerQuarter;

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, options, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    const std::vector<ReadEvent> meters =
        read.meta_of_type(vcc::music::smf_meta::kTimeSignature, 0);
    VCC_CHECK_EQ(meters.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(meters[0].tick, static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(meters[0].data[0], static_cast<std::uint8_t>(7));

    const std::vector<ReadEvent> tempos = read.meta_of_type(vcc::music::smf_meta::kTempo, 0);
    VCC_CHECK_EQ(tempos.size(), static_cast<std::size_t>(1));
    const std::int64_t microseconds = (static_cast<std::int64_t>(tempos[0].data[0]) << 16)
                                      | (static_cast<std::int64_t>(tempos[0].data[1]) << 8)
                                      | static_cast<std::int64_t>(tempos[0].data[2]);
    VCC_CHECK_EQ(microseconds, static_cast<std::int64_t>(416667));  // 144 bpm.
}

VCC_TEST(a_keyswitch_ahead_of_the_first_note_is_moved_to_the_start_not_dropped)
{
    // The renderer places a keyswitch before the note it applies to. When
    // that note is the first in the exported range, the switch would fall
    // before the file begins - and dropping it would play the note with the
    // wrong articulation, which is a silent wrong answer.
    //
    // The map is built here rather than taken from a default, so the case
    // tests the export rather than whichever articulations a default map
    // happens to carry.
    ExpressionMap map;
    ArticulationRule pizzicato;
    pizzicato.articulation = Articulation::Pizzicato;
    pizzicato.action = ArticulationAction::Keyswitch;
    pizzicato.keyswitch_note = 24;
    pizzicato.keyswitch_velocity = 100;
    VCC_CHECK(pizzicato.is_valid());
    map.set_rule(pizzicato);

    Score score;
    score.staff(0).name = "Strings";
    ScoreNote note = make_note(4 * kTicksPerQuarter, Step::C);
    note.articulations = static_cast<ArticulationMask>(Articulation::Pizzicato);
    score.insert_note(0, note);

    ScoreMidiOptions options;
    options.render.from_tick = 4 * kTicksPerQuarter;
    options.render.to_tick = 8 * kTicksPerQuarter;
    options.render.keyswitch_lead_ticks = 10;

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{map}, options, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    VCC_CHECK(built.keyswitch_count > 0);

    // The switch and its release, both of which the renderer put before the
    // note.
    VCC_CHECK_EQ(built.clamped_to_start, static_cast<std::size_t>(2));

    // The switch is in the file, at the start, before the note it applies
    // to - and it is released rather than left sounding for the rest of the
    // cue, which is what collapsing both onto tick 0 would cause.
    std::int64_t keyswitch_on = -1;
    std::int64_t keyswitch_off = -1;
    std::int64_t note_tick = -1;
    for (const ReadEvent& event : read.events) {
        if (event.is_meta || event.track != 1)
            continue;
        VCC_CHECK(event.tick >= 0);
        const bool is_attack = event.status_nibble() == 0x09 && event.data[2] > 0;
        const bool is_release =
            event.status_nibble() == 0x08 || (event.status_nibble() == 0x09 && event.data[2] == 0);
        if (event.data[1] == 24 && is_attack && keyswitch_on < 0)
            keyswitch_on = event.tick;
        else if (event.data[1] == 24 && is_release)
            keyswitch_off = event.tick;
        else if (event.data[1] == 60 && is_attack)
            note_tick = event.tick;
    }
    VCC_CHECK_EQ(keyswitch_on, static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(note_tick, static_cast<std::int64_t>(0));
    VCC_CHECK_MSG(keyswitch_off > keyswitch_on,
                  "the keyswitch is released before it is pressed, so it never stops");
}

VCC_TEST(a_keyswitch_with_room_ahead_of_it_keeps_its_own_position)
{
    // The counterpart: nothing is moved when nothing needs to be, so a
    // clamped count above zero means something really was at risk.
    ExpressionMap map;
    ArticulationRule pizzicato;
    pizzicato.articulation = Articulation::Pizzicato;
    pizzicato.action = ArticulationAction::Keyswitch;
    pizzicato.keyswitch_note = 24;
    VCC_CHECK(pizzicato.is_valid());
    map.set_rule(pizzicato);

    Score score;
    score.staff(0).name = "Strings";
    ScoreNote note = make_note(4 * kTicksPerQuarter, Step::C);
    note.articulations = static_cast<ArticulationMask>(Articulation::Pizzicato);
    score.insert_note(0, note);

    ScoreMidiOptions options;
    options.render.keyswitch_lead_ticks = 30;  // Whole score, so there is room.

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{map}, options, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    VCC_CHECK(built.keyswitch_count > 0);
    VCC_CHECK_EQ(built.clamped_to_start, static_cast<std::size_t>(0));

    for (const ReadEvent& event : read.events) {
        if (!event.is_meta && event.track == 1 && event.status_nibble() == 0x09
            && event.data[1] == 24 && event.data[2] > 0) {
            VCC_CHECK_EQ(event.tick, static_cast<std::int64_t>(to_host_ticks(4 * kTicksPerQuarter - 30)));
        }
    }
}

// ---------------------------------------------------------------------------
// What a MIDI file cannot carry
// ---------------------------------------------------------------------------

VCC_TEST(a_tempo_the_format_cannot_express_is_named_not_dropped)
{
    // The score model accepts any tempo above zero; the file stores
    // microseconds per quarter note in 24 bits. A tempo slow enough to
    // overflow that is representable in a score and not in a file, and the
    // difference is reported in words - an export a composer cannot trust is
    // worse than one that says what it could not carry.
    Score score = four_quarters();
    TempoMarking glacial;
    glacial.tick = 0;
    glacial.beats_per_minute = 0.000001;
    VCC_CHECK(glacial.is_valid());  // The score model's own answer.
    score.add_tempo(glacial);
    VCC_CHECK_EQ(score.tempos().size(), static_cast<std::size_t>(1));

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, ScoreMidiOptions{}, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    VCC_CHECK_EQ(built.unrepresentable.size(), static_cast<std::size_t>(1));
    VCC_CHECK(built.unrepresentable[0].find("tempo") != std::string::npos);
    VCC_CHECK(!built.carried_everything());

    // The rest of the export still happened: the notes are there, and the
    // file is well formed with no tempo event rather than a wrong one.
    VCC_CHECK(read.meta_of_type(vcc::music::smf_meta::kTempo, 0).empty());
    VCC_CHECK_EQ(read.note_ons(1).size(), static_cast<std::size_t>(4));
}

VCC_TEST(an_unusable_default_tempo_is_reported_rather_than_substituted)
{
    // Unlike the score's own markings this one comes straight from the
    // caller, so it is the likeliest of these to be wrong.
    ScoreMidiOptions options;
    options.default_tempo_bpm = 0.0;

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(four_quarters(), MidiRenderer{}, options, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    VCC_CHECK_EQ(built.unrepresentable.size(), static_cast<std::size_t>(1));
    VCC_CHECK(read.meta_of_type(vcc::music::smf_meta::kTempo, 0).empty());
}

VCC_TEST(the_score_model_already_rules_out_a_meter_or_key_the_format_cannot_carry)
{
    // Recorded because the export checks for both anyway, and a check that
    // cannot fire is worth knowing about: it is defence against a future
    // relaxation of the score model, not a path a score reaches today.
    Score score;
    VCC_CHECK(!score.set_time_signature(0, vcc::music::TimeSignature{4, 6}));
    VCC_CHECK(!score.set_time_signature(0, vcc::music::TimeSignature{200, 4}));
    VCC_CHECK(!score.set_key_signature(0, KeySignature{8, false}));
    VCC_CHECK(!score.set_key_signature(0, KeySignature{-8, false}));

    // So what it does hold always survives the export.
    VCC_CHECK(score.set_time_signature(0, vcc::music::TimeSignature{128, 128}));
    VCC_CHECK(score.set_key_signature(0, KeySignature{-7, true}));

    const ScoreMidiResult built =
        build_score_midi(score, MidiRenderer{}, ScoreMidiOptions{});
    VCC_CHECK(built.unrepresentable.empty());
}

VCC_TEST(an_unplayable_note_is_reported_through_to_the_caller)
{
    Score score;
    score.staff(0).name = "Piccolo";
    score.staff(0).octave_shift = 8;  // Far out of MIDI's range once applied.
    score.insert_note(0, make_note(0, Step::C, 7));

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, ScoreMidiOptions{}, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    VCC_CHECK_EQ(built.out_of_range_notes.size(), static_cast<std::size_t>(1));
    VCC_CHECK(!built.carried_everything());
    VCC_CHECK(read.note_ons(1).empty());
}

VCC_TEST(an_empty_score_still_writes_a_readable_file)
{
    // A conductor track and an empty music track, both well formed. A DAW
    // offered a malformed file would refuse the whole import.
    Score score;

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, ScoreMidiOptions{}, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);
    VCC_CHECK_EQ(read.track_count, 2);
    VCC_CHECK_EQ(built.note_count, static_cast<std::size_t>(0));
    VCC_CHECK(built.carried_everything());
}

// ---------------------------------------------------------------------------
// The notes themselves
// ---------------------------------------------------------------------------

VCC_TEST(a_tick_between_the_files_ticks_is_rounded_to_the_nearest_and_counted)
{
    // The score counts three ticks to each of the file's. A note at a tick
    // that is not a multiple of three - which only a nested tuplet puts
    // there - goes to the nearer file tick, and is counted so the export
    // can say so.
    Score score;
    score.insert_note(0, make_note(0, Step::C, 4, quarter()));
    score.insert_note(0, make_note(kTicksPerQuarter + 1, Step::D, 4, quarter()));
    score.insert_note(0, make_note(2 * kTicksPerQuarter + 2, Step::E, 4, quarter()));

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, ScoreMidiOptions{}, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    const std::vector<ReadEvent> notes = read.note_ons(1);
    VCC_CHECK_EQ(notes.size(), static_cast<std::size_t>(3));
    if (notes.size() == 3) {
        VCC_CHECK_EQ(notes[1].tick, static_cast<std::int64_t>(vcc::music::kTicksPerQuarter));
        VCC_CHECK_EQ(notes[2].tick, static_cast<std::int64_t>(2 * vcc::music::kTicksPerQuarter + 1));
    }
    // The two note-ons, and their note-offs, which sit a tick off too.
    VCC_CHECK_EQ(built.rounded_to_file_ticks, static_cast<std::size_t>(4));
}

VCC_TEST(notes_land_on_the_ticks_they_were_written_at)
{
    // Acceptance criterion 2: entering a note results in correctly timed
    // MIDI. Here, all the way through to the bytes on disk.
    Score score;
    score.staff(0).name = "Strings";
    score.insert_note(0, make_note(0, Step::C, 4, quarter()));
    score.insert_note(0, make_note(kTicksPerQuarter, Step::D, 4, eighth()));
    // A dotted triplet sixteenth: the case floating-point time gets wrong.
    Duration awkward = eighth(1);
    awkward.tuplet = Tuplet{3, 2};
    score.insert_note(0, make_note(kTicksPerQuarter + kTicksPerQuarter / 2, Step::E, 4,
                                   awkward));

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, ScoreMidiOptions{}, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    const std::vector<ReadEvent> notes = read.note_ons(1);
    VCC_CHECK_EQ(notes.size(), static_cast<std::size_t>(3));
    VCC_CHECK_EQ(notes[0].tick, static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(notes[1].tick, static_cast<std::int64_t>(to_host_ticks(kTicksPerQuarter)));
    VCC_CHECK_EQ(notes[2].tick,
                 static_cast<std::int64_t>(to_host_ticks(kTicksPerQuarter + kTicksPerQuarter / 2)));

    // The file counts the host's ticks, and nothing written here fell
    // between two of them.
    VCC_CHECK_EQ(built.rounded_to_file_ticks, static_cast<std::size_t>(0));

    // Pitches as written: C4, D4, E4 are 60, 62, 64.
    VCC_CHECK_EQ(notes[0].data[1], static_cast<std::uint8_t>(60));
    VCC_CHECK_EQ(notes[1].data[1], static_cast<std::uint8_t>(62));
    VCC_CHECK_EQ(notes[2].data[1], static_cast<std::uint8_t>(64));
}

VCC_TEST(a_transposing_staff_exports_what_it_sounds_not_what_it_is_written_as)
{
    // A B-flat clarinet part sounds a major second below the written pitch,
    // and MIDI carries sounding pitch.
    Score score;
    score.staff(0).name = "Clarinet in Bb";
    score.staff(0).transposition_semitones = -2;
    score.insert_note(0, make_note(0, Step::C, 4));

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, ScoreMidiOptions{}, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    const std::vector<ReadEvent> notes = read.note_ons(1);
    VCC_CHECK_EQ(notes.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(notes[0].data[1], static_cast<std::uint8_t>(58));  // B-flat 3.
}

VCC_TEST(every_note_that_sounds_is_released)
{
    // A note-on with no matching release is a note that never stops, which
    // an importing DAW will faithfully reproduce.
    Score score = four_quarters();
    score.insert_note(0, make_note(0, Step::G, 3, whole()));  // Overlapping.

    ScoreMidiResult built;
    std::string error;
    const ParsedSmf read = round_trip(score, MidiRenderer{}, ScoreMidiOptions{}, built, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    std::vector<int> sounding(128, 0);
    for (const ReadEvent& event : read.events) {
        if (event.is_meta || event.track != 1)
            continue;
        const std::uint8_t nibble = event.status_nibble();
        if (nibble == 0x09 && event.data[2] > 0)
            ++sounding[event.data[1]];
        else if (nibble == 0x08 || (nibble == 0x09 && event.data[2] == 0))
            --sounding[event.data[1]];
    }
    for (std::size_t note = 0; note < sounding.size(); ++note) {
        VCC_CHECK_MSG(sounding[note] == 0,
                      "note " + std::to_string(note) + " is left sounding");
    }
}

VCC_TEST(building_without_writing_gives_the_same_file)
{
    // build_score_midi is the part worth calling on its own - to show the
    // export in the UI before committing it to disk.
    const Score score = four_quarters();
    const ScoreMidiResult built = build_score_midi(score, MidiRenderer{}, ScoreMidiOptions{});

    std::vector<std::uint8_t> bytes;
    VCC_CHECK(vcc::music::write_smf(built.file, bytes).ok);

    ScoreMidiResult written_report;
    std::string error;
    const ParsedSmf read =
        round_trip(score, MidiRenderer{}, ScoreMidiOptions{}, written_report, error);
    VCC_CHECK_MSG(read.ok, error + read.error);

    std::vector<std::uint8_t> from_write;
    VCC_CHECK(vcc::music::write_smf(written_report.file, from_write).ok);
    VCC_CHECK(bytes == from_write);
}

VCC_TEST(the_caller_s_renderer_is_left_alone)
{
    // Its channel and range are its own; a builder that reconfigured it
    // would change what the caller's next render produces.
    MidiRenderer renderer;
    RenderOptions original;
    original.channel = 9;
    original.from_tick = 123;
    renderer.set_options(original);

    ScoreMidiOptions options;
    options.first_channel = 3;
    build_score_midi(four_quarters(), renderer, options);

    VCC_CHECK_EQ(renderer.options().channel, static_cast<std::uint8_t>(9));
    VCC_CHECK_EQ(renderer.options().from_tick, static_cast<Ticks>(123));
}

VCC_TEST_MAIN("score midi file")
