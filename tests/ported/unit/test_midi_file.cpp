// Standard MIDI File writing (spec 01 section 7).
//
// Checked by parsing the bytes back, with a reader written here rather than
// against a table of expected bytes. A table written by the same hand as the
// writer agrees with the writer's mistakes; a reader that walks chunk
// lengths, delta times and status bytes has to be satisfied by a file that
// is actually well formed.
#include "vcc/music/MidiFile.h"

#include <cstdint>
#include <string>
#include <vector>

#include "vcc_test.h"
#include "vcc_test_files.h"

using namespace vcc::music;

namespace {

// ---------------------------------------------------------------------------
// A small independent reader
// ---------------------------------------------------------------------------

struct ReadEvent {
    std::size_t track = 0;
    std::int64_t tick = 0;       ///< Absolute, accumulated from the deltas.
    bool is_meta = false;
    std::uint8_t meta_type = 0;
    std::vector<std::uint8_t> data;   ///< Meta payload, or the message bytes.
};

struct ParsedSmf {
    bool ok = false;
    std::string error;
    int format = -1;
    int track_count = 0;
    int division = 0;
    std::vector<ReadEvent> events;

    std::vector<ReadEvent> in_track(std::size_t index) const
    {
        std::vector<ReadEvent> found;
        for (const ReadEvent& event : events) {
            if (event.track == index)
                found.push_back(event);
        }
        return found;
    }

    std::vector<ReadEvent> meta_of_type(std::uint8_t type) const
    {
        std::vector<ReadEvent> found;
        for (const ReadEvent& event : events) {
            if (event.is_meta && event.meta_type == type)
                found.push_back(event);
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

/// Named ParsedSmf rather than ReadFile, which is what it was called until
/// the suite was first cross-compiled for Windows: `ReadFile` is a Win32
/// function, and `windows.h` arrives in this translation unit through
/// vcc_test_files.h, so the unqualified name became ambiguous and the file
/// would not compile there at all.
ParsedSmf parse_smf(const std::vector<std::uint8_t>& bytes)
{
    ParsedSmf file;

    if (bytes.size() < 14) {
        file.error = "too short for a header";
        return file;
    }
    if (!matches_id(bytes, 0, "MThd")) {
        file.error = "no MThd";
        return file;
    }
    if (read_u32(bytes, 4) != 6) {
        file.error = "header length is not 6";
        return file;
    }

    file.format = read_u16(bytes, 8);
    file.track_count = read_u16(bytes, 10);
    file.division = read_u16(bytes, 12);

    std::size_t at = 14;
    for (int track = 0; track < file.track_count; ++track) {
        if (at + 8 > bytes.size()) {
            file.error = "a track chunk is missing";
            return file;
        }
        if (!matches_id(bytes, at, "MTrk")) {
            file.error = "no MTrk where one was expected";
            return file;
        }
        const std::size_t length = read_u32(bytes, at + 4);
        at += 8;
        const std::size_t end = at + length;
        if (end > bytes.size()) {
            file.error = "a track chunk runs past the end of the file";
            return file;
        }

        std::int64_t tick = 0;
        bool saw_end_of_track = false;
        while (at < end) {
            std::int64_t delta = 0;
            if (!read_variable_length(bytes.data(), end, at, delta)) {
                file.error = "a delta time is malformed";
                return file;
            }
            tick += delta;

            if (at >= end) {
                file.error = "a track ends in the middle of an event";
                return file;
            }

            ReadEvent event;
            event.track = static_cast<std::size_t>(track);
            event.tick = tick;

            const std::uint8_t status = bytes[at++];
            if (status == 0xFF) {
                if (at >= end) {
                    file.error = "a meta event has no type";
                    return file;
                }
                event.is_meta = true;
                event.meta_type = bytes[at++];
                std::int64_t payload = 0;
                if (!read_variable_length(bytes.data(), end, at, payload)) {
                    file.error = "a meta event's length is malformed";
                    return file;
                }
                if (at + static_cast<std::size_t>(payload) > end) {
                    file.error = "a meta event's payload runs past the track";
                    return file;
                }
                event.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                  bytes.begin()
                                      + static_cast<std::ptrdiff_t>(at + static_cast<std::size_t>(payload)));
                at += static_cast<std::size_t>(payload);

                if (event.meta_type == smf_meta::kEndOfTrack) {
                    saw_end_of_track = true;
                    if (at != end) {
                        file.error = "bytes follow the end-of-track event";
                        return file;
                    }
                }
            } else if ((status & 0x80) == 0) {
                // Running status. This writer never produces it, so meeting
                // one means the file is not what it claims to be.
                file.error = "running status, which this writer must not produce";
                return file;
            } else if (status >= 0xF0) {
                file.error = "a system message, which this writer must not produce";
                return file;
            } else {
                const std::uint8_t high = static_cast<std::uint8_t>(status >> 4);
                const std::size_t data_bytes =
                    (high == 0x0C || high == 0x0D) ? 1u : 2u;
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

        if (!saw_end_of_track) {
            file.error = "a track has no end-of-track event";
            return file;
        }
    }

    if (at != bytes.size()) {
        file.error = "trailing bytes after the last track";
        return file;
    }

    file.ok = true;
    return file;
}

/// Note-on events across a whole file, for a case that only cares how many
/// there are.
std::size_t read_back_note_count(const SmfFile& file)
{
    std::size_t count = 0;
    for (const SmfTrack& track : file.tracks) {
        for (const SmfEvent& event : track.events) {
            if (event.event.type == MidiEventType::NoteOn && event.event.data2 > 0)
                ++count;
            else if (event.event.type == MidiEventType::NoteOff)
                ++count;
        }
    }
    return count;
}

SmfFile one_note_file()
{
    SmfFile file;
    file.format = SmfFormat::SingleTrack;
    file.ticks_per_quarter = 960;

    SmfTrack track;
    track.name = "Strings";
    track.events.push_back(SmfEvent{0, make_note_on(0, 60, 96)});
    track.events.push_back(SmfEvent{960, make_note_off(0, 60)});
    file.tracks.push_back(std::move(track));
    return file;
}

}  // namespace

// ---------------------------------------------------------------------------
// Variable-length quantities
// ---------------------------------------------------------------------------

VCC_TEST(variable_length_quantities_round_trip)
{
    // The values the format's own specification gives as examples, plus the
    // boundaries either side of each byte-count step.
    const std::int64_t values[] = {0,      1,       0x7F,      0x80,       0x2000,
                                   0x3FFF, 0x4000,  0x100000,  0x1FFFFF,   0x200000,
                                   0x0FFFFFFF};

    for (const std::int64_t value : values) {
        std::vector<std::uint8_t> bytes;
        VCC_CHECK(append_variable_length(value, bytes));
        VCC_CHECK(!bytes.empty());
        VCC_CHECK(bytes.size() <= 4);

        std::size_t at = 0;
        std::int64_t read_back = -1;
        VCC_CHECK(read_variable_length(bytes.data(), bytes.size(), at, read_back));
        VCC_CHECK_EQ(read_back, value);
        VCC_CHECK_EQ(at, bytes.size());

        // Every byte but the last carries the continuation bit, and the last
        // does not. That is the encoding, and it is what a reader keys on.
        for (std::size_t i = 0; i + 1 < bytes.size(); ++i)
            VCC_CHECK((bytes[i] & 0x80) != 0);
        VCC_CHECK((bytes.back() & 0x80) == 0);
    }
}

VCC_TEST(a_quantity_the_format_cannot_hold_is_refused)
{
    std::vector<std::uint8_t> bytes;
    VCC_CHECK(!append_variable_length(-1, bytes));
    VCC_CHECK(!append_variable_length(kSmfMaxDeltaTicks + 1, bytes));
    VCC_CHECK(bytes.empty());

    // And a reader refuses a fifth continuation byte rather than accepting a
    // quantity the format cannot express.
    const std::uint8_t too_long[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
    std::size_t at = 0;
    std::int64_t value = 0;
    VCC_CHECK(!read_variable_length(too_long, sizeof(too_long), at, value));
}

VCC_TEST(a_quantity_that_runs_out_of_bytes_is_refused)
{
    const std::uint8_t truncated[] = {0x81};
    std::size_t at = 0;
    std::int64_t value = 0;
    VCC_CHECK(!read_variable_length(truncated, sizeof(truncated), at, value));
}

// ---------------------------------------------------------------------------
// The file as a whole
// ---------------------------------------------------------------------------

VCC_TEST(a_written_file_parses_back)
{
    std::vector<std::uint8_t> bytes;
    const SmfWriteResult written = write_smf(one_note_file(), bytes);
    VCC_CHECK(written.ok);
    VCC_CHECK_EQ(written.error, std::string());
    VCC_CHECK_EQ(written.track_count, static_cast<std::size_t>(1));
    VCC_CHECK_EQ(written.byte_count, bytes.size());
    VCC_CHECK_EQ(written.dropped_events, static_cast<std::size_t>(0));

    const ParsedSmf read = parse_smf(bytes);
    VCC_CHECK_MSG(read.ok, read.error);
    VCC_CHECK_EQ(read.format, 0);
    VCC_CHECK_EQ(read.track_count, 1);
    VCC_CHECK_EQ(read.division, 960);

    // Name, note-on, note-off, end of track.
    VCC_CHECK_EQ(read.events.size(), static_cast<std::size_t>(4));
    VCC_CHECK_EQ(written.event_count, read.events.size());
}

VCC_TEST(absolute_ticks_become_correct_delta_times)
{
    // The one thing about this format that is easiest to get wrong, and the
    // reason SmfEvent carries an absolute position: the caller never keeps a
    // running total.
    SmfFile file;
    file.format = SmfFormat::SingleTrack;
    SmfTrack track;
    track.events.push_back(SmfEvent{0, make_note_on(0, 60, 100)});
    track.events.push_back(SmfEvent{480, make_note_off(0, 60)});
    track.events.push_back(SmfEvent{7680, make_note_on(0, 67, 100)});
    track.events.push_back(SmfEvent{8640, make_note_off(0, 67)});
    file.tracks.push_back(std::move(track));

    std::vector<std::uint8_t> bytes;
    VCC_CHECK(write_smf(file, bytes).ok);

    const ParsedSmf read = parse_smf(bytes);
    VCC_CHECK_MSG(read.ok, read.error);

    std::vector<std::int64_t> ticks;
    for (const ReadEvent& event : read.events) {
        if (!event.is_meta)
            ticks.push_back(event.tick);
    }
    VCC_CHECK_EQ(ticks.size(), static_cast<std::size_t>(4));
    VCC_CHECK_EQ(ticks[0], static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(ticks[1], static_cast<std::int64_t>(480));
    VCC_CHECK_EQ(ticks[2], static_cast<std::int64_t>(7680));
    VCC_CHECK_EQ(ticks[3], static_cast<std::int64_t>(8640));

    // The end-of-track sits at the last event, not at tick 0.
    const std::vector<ReadEvent> ends = read.meta_of_type(smf_meta::kEndOfTrack);
    VCC_CHECK_EQ(ends.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(ends[0].tick, static_cast<std::int64_t>(8640));
}

VCC_TEST(events_given_out_of_order_are_written_in_order)
{
    SmfFile file;
    file.format = SmfFormat::SingleTrack;
    SmfTrack track;
    track.events.push_back(SmfEvent{1920, make_note_off(0, 60)});
    track.events.push_back(SmfEvent{0, make_note_on(0, 60, 90)});
    track.events.push_back(SmfEvent{960, make_control_change(0, 1, 40)});
    file.tracks.push_back(std::move(track));

    std::vector<std::uint8_t> bytes;
    VCC_CHECK(write_smf(file, bytes).ok);

    const ParsedSmf read = parse_smf(bytes);
    VCC_CHECK_MSG(read.ok, read.error);

    std::int64_t previous = -1;
    for (const ReadEvent& event : read.events) {
        VCC_CHECK(event.tick >= previous);
        previous = event.tick;
    }
}

VCC_TEST(a_note_off_precedes_a_note_on_at_the_same_tick)
{
    // A repeated note must re-articulate. If its predecessor's release is
    // written after the new attack, the new note is cut off by the old one -
    // an audible, and very confusing, wrong answer.
    SmfFile file;
    file.format = SmfFormat::SingleTrack;
    SmfTrack track;
    track.events.push_back(SmfEvent{960, make_note_on(0, 60, 90)});   // Given first.
    track.events.push_back(SmfEvent{960, make_note_off(0, 60)});
    file.tracks.push_back(std::move(track));

    std::vector<std::uint8_t> bytes;
    VCC_CHECK(write_smf(file, bytes).ok);
    const ParsedSmf read = parse_smf(bytes);
    VCC_CHECK_MSG(read.ok, read.error);

    std::vector<std::uint8_t> statuses;
    for (const ReadEvent& event : read.events) {
        if (!event.is_meta)
            statuses.push_back(static_cast<std::uint8_t>(event.data[0] >> 4));
    }
    VCC_CHECK_EQ(statuses.size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(statuses[0], static_cast<std::uint8_t>(0x08));  // Note off first.
    VCC_CHECK_EQ(statuses[1], static_cast<std::uint8_t>(0x09));
}

VCC_TEST(a_zero_velocity_note_on_is_ordered_as_the_note_off_it_is)
{
    // Every reader treats it as a release, so it has to be ordered as one.
    SmfFile file;
    file.format = SmfFormat::SingleTrack;
    SmfTrack track;
    track.events.push_back(SmfEvent{480, make_note_on(0, 64, 100)});
    track.events.push_back(SmfEvent{480, make_note_on(0, 64, 0)});
    file.tracks.push_back(std::move(track));

    std::vector<std::uint8_t> bytes;
    VCC_CHECK(write_smf(file, bytes).ok);
    const ParsedSmf read = parse_smf(bytes);
    VCC_CHECK_MSG(read.ok, read.error);

    std::vector<std::uint8_t> velocities;
    for (const ReadEvent& event : read.events) {
        if (!event.is_meta)
            velocities.push_back(event.data[2]);
    }
    VCC_CHECK_EQ(velocities.size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(velocities[0], static_cast<std::uint8_t>(0));
    VCC_CHECK_EQ(velocities[1], static_cast<std::uint8_t>(100));
}

VCC_TEST(a_meta_event_precedes_the_notes_it_governs)
{
    SmfFile file;
    SmfTrack track;
    track.events.push_back(SmfEvent{1920, make_note_on(0, 60, 90)});
    track.meta.push_back(smf_tempo(1920, 90.0));
    file.tracks.push_back(std::move(track));
    file.format = SmfFormat::SingleTrack;

    std::vector<std::uint8_t> bytes;
    VCC_CHECK(write_smf(file, bytes).ok);
    const ParsedSmf read = parse_smf(bytes);
    VCC_CHECK_MSG(read.ok, read.error);

    bool saw_tempo = false;
    for (const ReadEvent& event : read.events) {
        if (event.is_meta && event.meta_type == smf_meta::kTempo)
            saw_tempo = true;
        else if (!event.is_meta)
            VCC_CHECK_MSG(saw_tempo, "a note was written before the tempo at its own tick");
    }
    VCC_CHECK(saw_tempo);
}

VCC_TEST(the_track_name_is_the_first_thing_in_the_track)
{
    SmfFile file;
    SmfTrack track;
    track.name = "Violin I";
    track.meta.push_back(smf_tempo(0, 120.0));
    track.events.push_back(SmfEvent{0, make_note_on(0, 60, 90)});
    file.tracks.push_back(std::move(track));
    file.format = SmfFormat::SingleTrack;

    std::vector<std::uint8_t> bytes;
    VCC_CHECK(write_smf(file, bytes).ok);
    const ParsedSmf read = parse_smf(bytes);
    VCC_CHECK_MSG(read.ok, read.error);

    VCC_CHECK(!read.events.empty());
    VCC_CHECK(read.events[0].is_meta);
    VCC_CHECK_EQ(read.events[0].meta_type, smf_meta::kTrackName);
    const std::string name(read.events[0].data.begin(), read.events[0].data.end());
    VCC_CHECK_EQ(name, std::string("Violin I"));
}

VCC_TEST(a_track_with_no_name_carries_no_name_event)
{
    std::vector<std::uint8_t> bytes;
    SmfFile file;
    file.format = SmfFormat::SingleTrack;
    SmfTrack track;
    track.events.push_back(SmfEvent{0, make_note_on(0, 60, 90)});
    file.tracks.push_back(std::move(track));

    VCC_CHECK(write_smf(file, bytes).ok);
    const ParsedSmf read = parse_smf(bytes);
    VCC_CHECK_MSG(read.ok, read.error);
    VCC_CHECK(read.meta_of_type(smf_meta::kTrackName).empty());
}

// ---------------------------------------------------------------------------
// Meta events
// ---------------------------------------------------------------------------

VCC_TEST(a_tempo_is_written_as_microseconds_per_quarter_note)
{
    bool ok = false;
    const SmfMetaEvent meta = smf_tempo(0, 120.0, ok);
    VCC_CHECK(ok);
    VCC_CHECK_EQ(meta.data.size(), static_cast<std::size_t>(3));

    // 120 quarter notes per minute is half a second each.
    const std::int64_t microseconds = (static_cast<std::int64_t>(meta.data[0]) << 16)
                                      | (static_cast<std::int64_t>(meta.data[1]) << 8)
                                      | static_cast<std::int64_t>(meta.data[2]);
    VCC_CHECK_EQ(microseconds, static_cast<std::int64_t>(500000));

    bool ok_90 = false;
    const SmfMetaEvent slower = smf_tempo(0, 90.0, ok_90);
    VCC_CHECK(ok_90);
    const std::int64_t slower_us = (static_cast<std::int64_t>(slower.data[0]) << 16)
                                   | (static_cast<std::int64_t>(slower.data[1]) << 8)
                                   | static_cast<std::int64_t>(slower.data[2]);
    VCC_CHECK_EQ(slower_us, static_cast<std::int64_t>(666667));  // Rounded once.
}

VCC_TEST(a_tempo_the_format_cannot_express_is_refused_not_clamped)
{
    // The field is 24 bits of microseconds per quarter note, so both ends
    // are bounded. A clamped tempo would play at a speed nobody asked for.
    for (const double tempo : {0.0, -60.0, 1e-9, 1e12}) {
        bool ok = true;
        const SmfMetaEvent meta = smf_tempo(0, tempo, ok);
        VCC_CHECK_MSG(!ok, "a tempo of " + std::to_string(tempo) + " was accepted");
        VCC_CHECK(!is_valid(meta));
    }

    // And the writer leaves such an event out, counting it.
    SmfFile file;
    file.format = SmfFormat::SingleTrack;
    SmfTrack track;
    track.meta.push_back(smf_tempo(0, 0.0));
    track.events.push_back(SmfEvent{0, make_note_on(0, 60, 90)});
    file.tracks.push_back(std::move(track));

    std::vector<std::uint8_t> bytes;
    const SmfWriteResult written = write_smf(file, bytes);
    VCC_CHECK(written.ok);
    VCC_CHECK_EQ(written.dropped_events, static_cast<std::size_t>(1));
    VCC_CHECK(parse_smf(bytes).meta_of_type(smf_meta::kTempo).empty());
}

VCC_TEST(a_time_signature_stores_the_denominator_as_a_power_of_two)
{
    VCC_CHECK_EQ(smf_denominator_exponent(1), 0);
    VCC_CHECK_EQ(smf_denominator_exponent(4), 2);
    VCC_CHECK_EQ(smf_denominator_exponent(8), 3);
    VCC_CHECK_EQ(smf_denominator_exponent(16), 4);
    VCC_CHECK_EQ(smf_denominator_exponent(6), -1);
    VCC_CHECK_EQ(smf_denominator_exponent(0), -1);
    VCC_CHECK_EQ(smf_denominator_exponent(-4), -1);

    bool ok = false;
    const SmfMetaEvent meta = smf_time_signature(0, TimeSignature{7, 8}, ok);
    VCC_CHECK(ok);
    VCC_CHECK_EQ(meta.data.size(), static_cast<std::size_t>(4));
    VCC_CHECK_EQ(meta.data[0], static_cast<std::uint8_t>(7));
    VCC_CHECK_EQ(meta.data[1], static_cast<std::uint8_t>(3));  // 8 = 2^3.
    VCC_CHECK_EQ(meta.data[2], static_cast<std::uint8_t>(24));
    VCC_CHECK_EQ(meta.data[3], static_cast<std::uint8_t>(8));
}

VCC_TEST(a_meter_the_format_cannot_express_is_refused)
{
    bool ok = true;
    smf_time_signature(0, TimeSignature{4, 6}, ok);
    VCC_CHECK(!ok);

    ok = true;
    smf_time_signature(0, TimeSignature{0, 4}, ok);
    VCC_CHECK(!ok);
}

VCC_TEST(a_key_signature_is_written_as_signed_fifths)
{
    bool ok = false;
    const SmfMetaEvent flats = smf_key_signature(0, -6, false, ok);
    VCC_CHECK(ok);
    VCC_CHECK_EQ(flats.data.size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(static_cast<std::int8_t>(flats.data[0]), static_cast<std::int8_t>(-6));
    VCC_CHECK_EQ(flats.data[1], static_cast<std::uint8_t>(0));

    const SmfMetaEvent minor = smf_key_signature(0, 3, true, ok);
    VCC_CHECK(ok);
    VCC_CHECK_EQ(minor.data[1], static_cast<std::uint8_t>(1));

    ok = true;
    smf_key_signature(0, 8, false, ok);
    VCC_CHECK(!ok);
    ok = true;
    smf_key_signature(0, -8, false, ok);
    VCC_CHECK(!ok);
}

VCC_TEST(text_meta_events_carry_their_bytes_unchanged)
{
    // Meta events are length-prefixed, so nothing needs escaping and UTF-8
    // survives. A name with a quote or an accent in it is a real name.
    const std::string awkward = "Cor anglais \"solo\" — pi\xc3\xb9 mosso";
    SmfFile file;
    file.format = SmfFormat::SingleTrack;
    SmfTrack track;
    track.name = awkward;
    track.meta.push_back(smf_marker(480, "A"));
    track.events.push_back(SmfEvent{0, make_note_on(0, 60, 90)});
    file.tracks.push_back(std::move(track));

    std::vector<std::uint8_t> bytes;
    VCC_CHECK(write_smf(file, bytes).ok);
    const ParsedSmf read = parse_smf(bytes);
    VCC_CHECK_MSG(read.ok, read.error);

    const std::vector<ReadEvent> names = read.meta_of_type(smf_meta::kTrackName);
    VCC_CHECK_EQ(names.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(std::string(names[0].data.begin(), names[0].data.end()), awkward);

    const std::vector<ReadEvent> markers = read.meta_of_type(smf_meta::kMarker);
    VCC_CHECK_EQ(markers.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(markers[0].tick, static_cast<std::int64_t>(480));
}

VCC_TEST(an_empty_text_meta_event_is_legal)
{
    // Zero-length text is a valid payload, and treating it as malformed
    // would drop an event the caller meant.
    const SmfMetaEvent empty = smf_text(0, smf_meta::kText, "");
    VCC_CHECK(is_valid(empty));
}

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

VCC_TEST(an_invalid_event_is_left_out_and_counted)
{
    SmfFile file;
    file.format = SmfFormat::SingleTrack;
    SmfTrack track;
    track.events.push_back(SmfEvent{0, make_note_on(0, 60, 90)});

    MidiEvent broken{};  // Invalid type.
    track.events.push_back(SmfEvent{480, broken});

    MidiEvent out_of_range = make_note_on(0, 60, 90);
    out_of_range.data1 = 200;  // Not a MIDI note number.
    track.events.push_back(SmfEvent{960, out_of_range});

    file.tracks.push_back(std::move(track));

    std::vector<std::uint8_t> bytes;
    const SmfWriteResult written = write_smf(file, bytes);
    VCC_CHECK(written.ok);
    VCC_CHECK_EQ(written.dropped_events, static_cast<std::size_t>(2));

    const ParsedSmf read = parse_smf(bytes);
    VCC_CHECK_MSG(read.ok, read.error);
    std::size_t channel_events = 0;
    for (const ReadEvent& event : read.events) {
        if (!event.is_meta)
            ++channel_events;
    }
    VCC_CHECK_EQ(channel_events, static_cast<std::size_t>(1));
}

VCC_TEST(a_negative_position_is_refused_rather_than_written_as_a_huge_delta)
{
    SmfFile file;
    file.format = SmfFormat::SingleTrack;
    SmfTrack track;
    track.events.push_back(SmfEvent{-1, make_note_on(0, 60, 90)});
    file.tracks.push_back(std::move(track));

    std::vector<std::uint8_t> bytes;
    const SmfWriteResult written = write_smf(file, bytes);
    VCC_CHECK(!written.ok);
    VCC_CHECK(written.error.find("before the start") != std::string::npos);
    VCC_CHECK(bytes.empty());
}

VCC_TEST(a_gap_longer_than_the_format_allows_is_refused)
{
    SmfFile file;
    file.format = SmfFormat::SingleTrack;
    SmfTrack track;
    track.events.push_back(SmfEvent{0, make_note_on(0, 60, 90)});
    track.events.push_back(SmfEvent{kSmfMaxDeltaTicks + 1, make_note_off(0, 60)});
    file.tracks.push_back(std::move(track));

    std::vector<std::uint8_t> bytes;
    const SmfWriteResult written = write_smf(file, bytes);
    VCC_CHECK(!written.ok);
    VCC_CHECK(written.error.find("longer than the format") != std::string::npos);
}

VCC_TEST(a_caller_supplied_end_of_track_is_refused)
{
    // Writing a second one would put bytes after the end of the track, which
    // is a corrupt file rather than a data problem - so it is not something
    // to drop and count.
    SmfFile file;
    file.format = SmfFormat::SingleTrack;
    SmfTrack track;
    track.meta.push_back(SmfMetaEvent{0, smf_meta::kEndOfTrack, {}});
    file.tracks.push_back(std::move(track));

    std::vector<std::uint8_t> bytes;
    const SmfWriteResult written = write_smf(file, bytes);
    VCC_CHECK(!written.ok);
    VCC_CHECK(written.error.find("end-of-track") != std::string::npos);
}

VCC_TEST(a_bad_division_or_track_count_is_refused)
{
    {
        SmfFile file = one_note_file();
        file.ticks_per_quarter = 0;
        std::vector<std::uint8_t> bytes;
        VCC_CHECK(!write_smf(file, bytes).ok);
    }
    {
        SmfFile file = one_note_file();
        file.ticks_per_quarter = static_cast<std::uint16_t>(kSmfMaxDivision + 1);
        std::vector<std::uint8_t> bytes;
        VCC_CHECK(!write_smf(file, bytes).ok);
    }
    {
        SmfFile file;
        std::vector<std::uint8_t> bytes;
        const SmfWriteResult written = write_smf(file, bytes);
        VCC_CHECK(!written.ok);
        VCC_CHECK(written.error.find("at least one track") != std::string::npos);
    }
    {
        // Format 0 means one track by definition.
        SmfFile file = one_note_file();
        file.tracks.push_back(SmfTrack{});
        std::vector<std::uint8_t> bytes;
        const SmfWriteResult written = write_smf(file, bytes);
        VCC_CHECK(!written.ok);
        VCC_CHECK(written.error.find("format 0") != std::string::npos);
    }
}

VCC_TEST(an_empty_track_is_still_a_well_formed_track)
{
    // A conductor track for a score with nothing in it, for instance. It
    // needs its end-of-track event like any other.
    SmfFile file;
    file.format = SmfFormat::MultiTrack;
    file.tracks.push_back(SmfTrack{});
    file.tracks.push_back(one_note_file().tracks[0]);

    std::vector<std::uint8_t> bytes;
    VCC_CHECK(write_smf(file, bytes).ok);

    const ParsedSmf read = parse_smf(bytes);
    VCC_CHECK_MSG(read.ok, read.error);
    VCC_CHECK_EQ(read.format, 1);
    VCC_CHECK_EQ(read.track_count, 2);
    VCC_CHECK_EQ(read.in_track(0).size(), static_cast<std::size_t>(1));  // End of track.
}

VCC_TEST(several_tracks_each_keep_their_own_timeline)
{
    // Delta times are per track. A second track that continued the first
    // one's running total would play at the wrong time entirely.
    SmfFile file;
    file.format = SmfFormat::MultiTrack;

    SmfTrack first;
    first.name = "A";
    first.events.push_back(SmfEvent{3840, make_note_on(0, 60, 90)});
    file.tracks.push_back(std::move(first));

    SmfTrack second;
    second.name = "B";
    second.events.push_back(SmfEvent{480, make_note_on(1, 67, 90)});
    file.tracks.push_back(std::move(second));

    std::vector<std::uint8_t> bytes;
    VCC_CHECK(write_smf(file, bytes).ok);

    const ParsedSmf read = parse_smf(bytes);
    VCC_CHECK_MSG(read.ok, read.error);

    for (const ReadEvent& event : read.in_track(1)) {
        if (!event.is_meta)
            VCC_CHECK_EQ(event.tick, static_cast<std::int64_t>(480));
    }
}

VCC_TEST(every_event_carries_its_own_status_byte)
{
    // Running status would save a byte per event and is the part of this
    // format readers most often get wrong. The parser above refuses a file
    // that uses it, so this passing at all is the assertion - but a run of
    // same-channel notes is where a writer would be tempted.
    SmfFile file;
    file.format = SmfFormat::SingleTrack;
    SmfTrack track;
    for (int i = 0; i < 8; ++i) {
        track.events.push_back(
            SmfEvent{i * 240, make_note_on(0, static_cast<std::uint8_t>(60 + i), 90)});
        track.events.push_back(
            SmfEvent{i * 240 + 120, make_note_off(0, static_cast<std::uint8_t>(60 + i))});
    }
    file.tracks.push_back(std::move(track));

    std::vector<std::uint8_t> bytes;
    VCC_CHECK(write_smf(file, bytes).ok);
    const ParsedSmf read = parse_smf(bytes);
    VCC_CHECK_MSG(read.ok, read.error);
    VCC_CHECK_EQ(read.events.size(), static_cast<std::size_t>(17));
}

VCC_TEST(every_event_type_survives_the_round_trip)
{
    SmfFile file;
    file.format = SmfFormat::SingleTrack;
    SmfTrack track;
    track.events.push_back(SmfEvent{0, make_note_on(3, 60, 90)});
    track.events.push_back(SmfEvent{10, make_control_change(3, 11, 64)});
    track.events.push_back(SmfEvent{20, make_program_change(3, 42)});
    track.events.push_back(SmfEvent{30, make_pitch_bend(3, 12000)});
    track.events.push_back(SmfEvent{40, make_channel_pressure(3, 55)});
    track.events.push_back(SmfEvent{50, make_poly_pressure(3, 60, 33)});
    track.events.push_back(SmfEvent{60, make_note_off(3, 60, 12)});
    file.tracks.push_back(std::move(track));

    std::vector<std::uint8_t> bytes;
    const SmfWriteResult written = write_smf(file, bytes);
    VCC_CHECK(written.ok);
    VCC_CHECK_EQ(written.dropped_events, static_cast<std::size_t>(0));

    const ParsedSmf read = parse_smf(bytes);
    VCC_CHECK_MSG(read.ok, read.error);

    std::vector<ReadEvent> channel;
    for (const ReadEvent& event : read.events) {
        if (!event.is_meta)
            channel.push_back(event);
    }
    VCC_CHECK_EQ(channel.size(), static_cast<std::size_t>(7));

    // Every message on channel 3, and the two one-byte messages a byte
    // shorter than the rest.
    for (const ReadEvent& event : channel)
        VCC_CHECK_EQ(static_cast<std::uint8_t>(event.data[0] & 0x0F), static_cast<std::uint8_t>(3));

    VCC_CHECK_EQ(channel[2].data.size(), static_cast<std::size_t>(2));  // Program change.
    VCC_CHECK_EQ(channel[4].data.size(), static_cast<std::size_t>(2));  // Channel pressure.

    // Pitch bend is little-endian in MIDI: low seven bits first.
    VCC_CHECK_EQ(channel[3].data[1], static_cast<std::uint8_t>(12000 & 0x7F));
    VCC_CHECK_EQ(channel[3].data[2], static_cast<std::uint8_t>((12000 >> 7) & 0x7F));

    VCC_CHECK_EQ(channel[6].data[2], static_cast<std::uint8_t>(12));  // Release velocity.
}

VCC_TEST(event_and_track_counts_are_reported_honestly)
{
    SmfFile file;
    file.format = SmfFormat::MultiTrack;

    SmfTrack conductor;
    conductor.name = "Conductor";
    conductor.meta.push_back(smf_tempo(0, 100.0));
    conductor.meta.push_back(smf_time_signature(0, TimeSignature{3, 4}));
    file.tracks.push_back(std::move(conductor));

    SmfTrack music;
    music.name = "Strings";
    music.events.push_back(SmfEvent{0, make_note_on(0, 60, 90)});
    music.events.push_back(SmfEvent{960, make_note_off(0, 60)});
    file.tracks.push_back(std::move(music));

    VCC_CHECK_EQ(file.event_count(), static_cast<std::size_t>(4));

    std::vector<std::uint8_t> bytes;
    const SmfWriteResult written = write_smf(file, bytes);
    VCC_CHECK(written.ok);
    VCC_CHECK_EQ(written.track_count, static_cast<std::size_t>(2));

    // The four given, the two names, and one end-of-track per track.
    VCC_CHECK_EQ(written.event_count, static_cast<std::size_t>(8));
    VCC_CHECK_EQ(written.event_count, parse_smf(bytes).events.size());
}

// ---------------------------------------------------------------------------
// Writing to disk
// ---------------------------------------------------------------------------

VCC_TEST(writing_to_a_file_produces_the_same_bytes)
{
    const vcc::test::TempFile file("smf", ".mid");

    std::vector<std::uint8_t> expected;
    VCC_CHECK(write_smf(one_note_file(), expected).ok);

    const SmfWriteResult written = write_smf_file(one_note_file(), file.path());
    VCC_CHECK_MSG(written.ok, written.error);
    VCC_CHECK_EQ(written.byte_count, expected.size());

    std::vector<std::uint8_t> read_back;
    VCC_CHECK(vcc::test::read_file(file.path(), read_back));
    VCC_CHECK_EQ(read_back.size(), expected.size());
    VCC_CHECK(read_back == expected);

    // And the bytes on disk parse as a MIDI file, not just as the same bytes
    // this writer produced in memory.
    const ParsedSmf parsed = parse_smf(read_back);
    VCC_CHECK_MSG(parsed.ok, parsed.error);
}

VCC_TEST(a_refused_file_leaves_nothing_behind)
{
    // Pro Tools must never be handed a truncated file, so the bytes are
    // built before the file is opened at all.
    const vcc::test::TempFile target("smf-refused", ".mid");

    SmfFile file = one_note_file();
    file.tracks[0].events.push_back(SmfEvent{-5, make_note_off(0, 60)});

    const SmfWriteResult written = write_smf_file(file, target.path());
    VCC_CHECK(!written.ok);

    std::vector<std::uint8_t> bytes;
    VCC_CHECK_MSG(!vcc::test::read_file(target.path(), bytes), "a file was left behind");
}

VCC_TEST(a_path_that_cannot_be_opened_is_reported_without_echoing_it)
{
    // The message reaches logs, and a path can carry a user's directory
    // layout (spec 01 sections 9, 10).
    const std::string path = "/nonexistent-directory-for-vcc/cue.mid";
    const SmfWriteResult written = write_smf_file(one_note_file(), path);
    VCC_CHECK(!written.ok);
    VCC_CHECK(!written.error.empty());
    VCC_CHECK(written.error.find("nonexistent-directory-for-vcc") == std::string::npos);

    VCC_CHECK(!write_smf_file(one_note_file(), "").ok);
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

VCC_TEST(a_written_file_reads_back_into_the_same_file)
{
    // The strongest statement available about the reader: what it produces,
    // written again, is byte-identical. A field the writer emits and the
    // reader drops shows up here and nowhere else.
    SmfFile original;
    original.format = SmfFormat::MultiTrack;
    original.ticks_per_quarter = 960;

    SmfTrack conductor;
    conductor.name = "Conductor";
    conductor.meta.push_back(smf_tempo(0, 96.0));
    conductor.meta.push_back(smf_time_signature(0, TimeSignature{7, 8}));
    conductor.meta.push_back(smf_key_signature(0, -3, true));
    conductor.meta.push_back(smf_marker(1920, "A"));
    conductor.meta.push_back(smf_text(0, smf_meta::kCopyright, "(c) a composer"));
    original.tracks.push_back(std::move(conductor));

    SmfTrack music;
    music.name = "Violin I";
    for (int i = 0; i < 6; ++i) {
        music.events.push_back(
            SmfEvent{i * 240, make_note_on(3, static_cast<std::uint8_t>(60 + i), 90)});
        music.events.push_back(
            SmfEvent{i * 240 + 200, make_note_off(3, static_cast<std::uint8_t>(60 + i), 17)});
    }
    music.events.push_back(SmfEvent{0, make_control_change(3, 11, 64)});
    music.events.push_back(SmfEvent{100, make_pitch_bend(3, 12000)});
    music.events.push_back(SmfEvent{110, make_program_change(3, 42)});
    music.events.push_back(SmfEvent{120, make_channel_pressure(3, 55)});
    music.events.push_back(SmfEvent{130, make_poly_pressure(3, 60, 33)});
    original.tracks.push_back(std::move(music));

    std::vector<std::uint8_t> first;
    VCC_CHECK(write_smf(original, first).ok);

    SmfFile read_back;
    const SmfReadResult read = read_smf(first, read_back);
    VCC_CHECK_MSG(read.ok, read.error);
    VCC_CHECK_EQ(read.format, 1);
    VCC_CHECK_EQ(read.track_count, static_cast<std::size_t>(2));
    VCC_CHECK_EQ(read.skipped_events, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(read.byte_count, first.size());

    // Every event in the file, counted. The conductor track: a name, five
    // meta events, an end-of-track. The other: a name, twelve notes, five
    // controllers, an end-of-track.
    VCC_CHECK_EQ(read.event_count, static_cast<std::size_t>((1 + 5 + 1) + (1 + 17 + 1)));
    VCC_CHECK_EQ(read_back.ticks_per_quarter, static_cast<std::uint16_t>(960));
    VCC_CHECK_EQ(read_back.tracks.size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(read_back.tracks[0].name, std::string("Conductor"));
    VCC_CHECK_EQ(read_back.tracks[1].name, std::string("Violin I"));

    std::vector<std::uint8_t> second;
    VCC_CHECK(write_smf(read_back, second).ok);
    VCC_CHECK_EQ(second.size(), first.size());
    VCC_CHECK_MSG(second == first, "the file changed by being read and written");
}

VCC_TEST(running_status_is_accepted_even_though_it_is_never_written)
{
    // The writer refuses to emit it; the reader has to accept it, because
    // real files use it everywhere. Be strict in what you send.
    std::vector<std::uint8_t> bytes = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0,  // 480 ppqn, format 0.
        'M', 'T', 'r', 'k', 0, 0, 0, 0,                           // Length patched below.
    };
    const std::size_t length_at = bytes.size() - 4;

    const std::vector<std::uint8_t> track = {
        0x00, 0x90, 60, 100,   // Note on, with a status byte.
        0x10, 62, 100,         // Running status: another note on.
        0x10, 64, 100,         // And another.
        0x20, 0x80, 60, 0,     // Note off, with a status byte.
        0x00, 62, 0,           // Running status: another note off.
        0x00, 64, 0,
        0x00, 0xFF, 0x2F, 0x00,
    };
    bytes.insert(bytes.end(), track.begin(), track.end());
    const std::uint32_t track_length = static_cast<std::uint32_t>(track.size());
    bytes[length_at] = static_cast<std::uint8_t>((track_length >> 24) & 0xFF);
    bytes[length_at + 1] = static_cast<std::uint8_t>((track_length >> 16) & 0xFF);
    bytes[length_at + 2] = static_cast<std::uint8_t>((track_length >> 8) & 0xFF);
    bytes[length_at + 3] = static_cast<std::uint8_t>(track_length & 0xFF);

    SmfFile file;
    const SmfReadResult read = read_smf(bytes, file);
    VCC_CHECK_MSG(read.ok, read.error);
    VCC_CHECK_EQ(read_back_note_count(file), static_cast<std::size_t>(6));
    VCC_CHECK_EQ(file.ticks_per_quarter, static_cast<std::uint16_t>(480));
    VCC_CHECK_EQ(file.tracks.size(), static_cast<std::size_t>(1));

    // Three attacks at 0, 16 and 32 of the file's own ticks, and their
    // releases a quarter of a beat later.
    const std::vector<SmfEvent>& events = file.tracks[0].events;
    VCC_CHECK_EQ(events.size(), static_cast<std::size_t>(6));
    if (events.size() == 6) {
        VCC_CHECK_EQ(events[0].tick, static_cast<std::int64_t>(0));
        VCC_CHECK_EQ(events[0].event.data1, static_cast<std::uint8_t>(60));
        VCC_CHECK_EQ(events[1].tick, static_cast<std::int64_t>(16));
        VCC_CHECK_EQ(events[1].event.data1, static_cast<std::uint8_t>(62));
        VCC_CHECK_EQ(events[2].tick, static_cast<std::int64_t>(32));
        VCC_CHECK_EQ(events[3].tick, static_cast<std::int64_t>(64));
        VCC_CHECK_EQ(events[3].event.type, MidiEventType::NoteOff);
        VCC_CHECK_EQ(events[5].event.data1, static_cast<std::uint8_t>(64));
    }

    // The end-of-track was consumed, not kept: the writer owns it.
    for (const SmfMetaEvent& meta : file.tracks[0].meta)
        VCC_CHECK(meta.type != smf_meta::kEndOfTrack);

    // And it writes back out as a well-formed file, with status bytes.
    std::vector<std::uint8_t> written;
    VCC_CHECK(write_smf(file, written).ok);
    VCC_CHECK(parse_smf(written).ok);
}

VCC_TEST(a_meta_event_does_not_become_the_running_status)
{
    // A meta event between two running-status notes must not be remembered
    // as the status to run on. Reading the second note as a meta event
    // would take its pitch for a type and its velocity for a length, and
    // the rest of the track with it - so this is the difference between a
    // part and nonsense, on a file shape that is entirely ordinary.
    std::vector<std::uint8_t> bytes = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x03, 0xC0,  // 960 ppqn, format 0.
        'M', 'T', 'r', 'k', 0, 0, 0, 0,
    };
    const std::size_t length_at = bytes.size() - 4;

    const std::vector<std::uint8_t> track = {
        0x00, 0x90, 60, 100,               // Note on, with a status byte.
        0x00, 0xFF, 0x06, 0x02, 'h', 'i',  // A marker in between.
        0x10, 62, 100,                     // Running status: another note on.
        0x10, 0x80, 60, 0,
        0x00, 62, 0,                       // Running status: another note off.
        0x00, 0xFF, 0x2F, 0x00,
    };
    bytes.insert(bytes.end(), track.begin(), track.end());
    const auto track_length = static_cast<std::uint32_t>(track.size());
    bytes[length_at] = static_cast<std::uint8_t>((track_length >> 24) & 0xFF);
    bytes[length_at + 1] = static_cast<std::uint8_t>((track_length >> 16) & 0xFF);
    bytes[length_at + 2] = static_cast<std::uint8_t>((track_length >> 8) & 0xFF);
    bytes[length_at + 3] = static_cast<std::uint8_t>(track_length & 0xFF);

    SmfFile file;
    const SmfReadResult read = read_smf(bytes, file);
    VCC_CHECK_MSG(read.ok, read.error);
    VCC_CHECK_EQ(file.tracks.size(), static_cast<std::size_t>(1));
    if (file.tracks.empty())
        return;

    const std::vector<SmfEvent>& events = file.tracks[0].events;
    VCC_CHECK_EQ(events.size(), static_cast<std::size_t>(4));
    if (events.size() == 4) {
        VCC_CHECK_EQ(events[0].event.data1, static_cast<std::uint8_t>(60));
        VCC_CHECK_EQ(events[1].tick, static_cast<std::int64_t>(16));
        VCC_CHECK_EQ(events[1].event.type, MidiEventType::NoteOn);
        VCC_CHECK_EQ(events[1].event.data1, static_cast<std::uint8_t>(62));
        VCC_CHECK_EQ(events[3].event.type, MidiEventType::NoteOff);
    }

    // The marker survived as a marker, and is the only meta event kept.
    VCC_CHECK_EQ(file.tracks[0].meta.size(), static_cast<std::size_t>(1));
    if (!file.tracks[0].meta.empty()) {
        VCC_CHECK_EQ(file.tracks[0].meta.front().type, smf_meta::kMarker);
        VCC_CHECK_EQ(std::string(file.tracks[0].meta.front().data.begin(),
                                 file.tracks[0].meta.front().data.end()),
                     std::string("hi"));
    }
}

VCC_TEST(a_chunk_that_runs_past_the_end_of_the_file_is_refused)
{
    // A truncated download, or a length field that lies. Reading the track
    // anyway would run off the end of the buffer, so this is a refusal
    // rather than a best effort.
    const std::vector<std::uint8_t> bytes = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x03, 0xC0,
        'M', 'T', 'r', 'k', 0, 0, 0x10, 0x00,  // Claims 4096 bytes.
        0x00, 0x90, 60, 100,                   // Has four.
    };

    SmfFile file;
    const SmfReadResult read = read_smf(bytes, file);
    VCC_CHECK(!read.ok);
    VCC_CHECK(read.error.find("past the end") != std::string::npos);
    VCC_CHECK(file.tracks.empty());
}

VCC_TEST(a_header_with_no_tracks_after_it_is_refused)
{
    // There is no music in it, and a caller handed an empty file would have
    // to invent the difference between "nothing in it" and "could not read
    // it" for itself.
    const std::vector<std::uint8_t> bytes = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x03, 0xC0,
    };

    SmfFile file;
    const SmfReadResult read = read_smf(bytes, file);
    VCC_CHECK(!read.ok);
    VCC_CHECK(!read.error.empty());
    VCC_CHECK(file.tracks.empty());
    VCC_CHECK_EQ(read.track_count, static_cast<std::size_t>(0));
}

VCC_TEST(what_cannot_become_musical_time_is_refused_not_guessed)
{
    const auto header = [](std::uint16_t format, std::uint16_t tracks,
                           std::uint16_t division) {
        return std::vector<std::uint8_t>{
            'M', 'T', 'h', 'd', 0, 0, 0, 6,
            static_cast<std::uint8_t>(format >> 8), static_cast<std::uint8_t>(format & 0xFF),
            static_cast<std::uint8_t>(tracks >> 8), static_cast<std::uint8_t>(tracks & 0xFF),
            static_cast<std::uint8_t>(division >> 8),
            static_cast<std::uint8_t>(division & 0xFF),
            'M', 'T', 'r', 'k', 0, 0, 0, 4, 0x00, 0xFF, 0x2F, 0x00,
        };
    };

    SmfFile file;

    // SMPTE division: frames, not beats. There is no musical time in it.
    SmfReadResult read = read_smf(header(0, 1, 0xE728), file);
    VCC_CHECK(!read.ok);
    VCC_CHECK(read.error.find("SMPTE") != std::string::npos);

    // Format 2: independent sequences, not parts of one piece.
    read = read_smf(header(2, 1, 960), file);
    VCC_CHECK(!read.ok);
    VCC_CHECK(read.error.find("format 2") != std::string::npos);

    // No ticks in a quarter note at all.
    read = read_smf(header(0, 1, 0), file);
    VCC_CHECK(!read.ok);

    // Not a MIDI file, and too short to be one.
    read = read_smf({'n', 'o', 't', ' ', 'm', 'i', 'd', 'i'}, file);
    VCC_CHECK(!read.ok);
    VCC_CHECK(read.error.find("not a standard MIDI file") != std::string::npos);

    // Not a MIDI file, and long enough that only the magic says so. An AIFF
    // or a WAV picked out of the wrong folder is this case, and it is the
    // one the check exists for.
    const std::string prose = "this is prose, not a standard MIDI file at all";
    read = read_smf(std::vector<std::uint8_t>(prose.begin(), prose.end()), file);
    VCC_CHECK(!read.ok);
    VCC_CHECK(read.error.find("not a standard MIDI file") != std::string::npos);

    // And nothing is left behind by a refusal.
    VCC_CHECK(file.tracks.empty());
}

VCC_TEST(a_system_exclusive_block_is_stepped_over_and_counted)
{
    // There is nowhere in this representation to keep one, and silently
    // reading past it would make a file look smaller than it is.
    std::vector<std::uint8_t> bytes = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x03, 0xC0,
        'M', 'T', 'r', 'k', 0, 0, 0, 0,
    };
    const std::size_t length_at = bytes.size() - 4;
    const std::vector<std::uint8_t> track = {
        0x00, 0xF0, 0x04, 0x41, 0x10, 0x42, 0xF7,  // SysEx, four bytes of payload.
        0x00, 0x90, 60, 100,
        0x40, 0x80, 60, 0,
        0x00, 0xFF, 0x2F, 0x00,
    };
    bytes.insert(bytes.end(), track.begin(), track.end());
    bytes[length_at + 3] = static_cast<std::uint8_t>(track.size());

    SmfFile file;
    const SmfReadResult read = read_smf(bytes, file);
    VCC_CHECK_MSG(read.ok, read.error);
    VCC_CHECK_EQ(read.skipped_events, static_cast<std::size_t>(1));
    VCC_CHECK_EQ(file.tracks.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(file.tracks[0].events.size(), static_cast<std::size_t>(2));
}

VCC_TEST(an_unknown_chunk_is_ignored_as_the_format_requires)
{
    // Unknown chunks are to be skipped, which is how the format stays
    // extensible. A caller is still told there was one.
    std::vector<std::uint8_t> bytes = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x03, 0xC0,
        'X', 'Y', 'Z', 'Q', 0, 0, 0, 3, 1, 2, 3,
        'M', 'T', 'r', 'k', 0, 0, 0, 8,
        0x00, 0x90, 60, 100, 0x40, 0xFF, 0x2F, 0x00,
    };

    SmfFile file;
    const SmfReadResult read = read_smf(bytes, file);
    VCC_CHECK_MSG(read.ok, read.error);
    VCC_CHECK_EQ(file.tracks.size(), static_cast<std::size_t>(1));
    VCC_CHECK(read.skipped_events >= 1);
}

VCC_TEST(a_malformed_file_is_refused_rather_than_partly_read)
{
    SmfFile file;

    // A chunk longer than the bytes that follow it.
    std::vector<std::uint8_t> short_chunk = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x03, 0xC0,
        'M', 'T', 'r', 'k', 0, 0, 0x10, 0x00, 0x00,
    };
    SmfReadResult read = read_smf(short_chunk, file);
    VCC_CHECK(!read.ok);
    VCC_CHECK(file.tracks.empty());

    // A header that claims to be shorter than a header.
    std::vector<std::uint8_t> bad_header = {
        'M', 'T', 'h', 'd', 0, 0, 0, 2, 0, 0, 0, 1, 0x03, 0xC0,
    };
    VCC_CHECK(!read_smf(bad_header, file).ok);

    // A track that ends in the middle of an event.
    std::vector<std::uint8_t> truncated = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x03, 0xC0,
        'M', 'T', 'r', 'k', 0, 0, 0, 2, 0x00, 0x90,
    };
    VCC_CHECK(!read_smf(truncated, file).ok);

    // Nothing at all.
    VCC_CHECK(!read_smf({}, file).ok);
}

VCC_TEST(a_track_that_declares_no_terminator_is_still_music)
{
    // The format requires an end-of-track event. Refusing a file over a
    // missing terminator would refuse music that is otherwise intact.
    std::vector<std::uint8_t> bytes = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x03, 0xC0,
        'M', 'T', 'r', 'k', 0, 0, 0, 8,
        0x00, 0x90, 60, 100, 0x40, 0x80, 60, 0,
    };

    SmfFile file;
    const SmfReadResult read = read_smf(bytes, file);
    VCC_CHECK_MSG(read.ok, read.error);
    VCC_CHECK_EQ(file.tracks.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(file.tracks[0].events.size(), static_cast<std::size_t>(2));
}

VCC_TEST(a_file_that_disagrees_with_itself_about_its_track_count_is_read_anyway)
{
    std::vector<std::uint8_t> bytes = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 9,  // Claims nine tracks.
        0x03, 0xC0,
        'M', 'T', 'r', 'k', 0, 0, 0, 8,
        0x00, 0x90, 60, 100, 0x40, 0xFF, 0x2F, 0x00,
    };

    SmfFile file;
    const SmfReadResult read = read_smf(bytes, file);
    VCC_CHECK_MSG(read.ok, read.error);
    VCC_CHECK_EQ(file.tracks.size(), static_cast<std::size_t>(1));
    VCC_CHECK(read.skipped_events >= 1);
}

VCC_TEST(reading_a_file_that_is_not_there_is_reported_without_echoing_it)
{
    SmfFile file;
    const SmfReadResult read =
        read_smf_file("/nonexistent-directory-for-vcc/cue.mid", file);
    VCC_CHECK(!read.ok);
    VCC_CHECK(!read.error.empty());
    VCC_CHECK(read.error.find("nonexistent-directory-for-vcc") == std::string::npos);
    VCC_CHECK(!read_smf_file("", file).ok);
}

VCC_TEST(a_file_on_disk_reads_back_as_what_was_written)
{
    const vcc::test::TempFile file("smf-read", ".mid");
    VCC_CHECK(write_smf_file(one_note_file(), file.path()).ok);

    SmfFile read_back;
    const SmfReadResult read = read_smf_file(file.path(), read_back);
    VCC_CHECK_MSG(read.ok, read.error);
    VCC_CHECK_EQ(read_back.tracks.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(read_back.tracks[0].name, std::string("Strings"));
    VCC_CHECK_EQ(read_back.tracks[0].events.size(), static_cast<std::size_t>(2));

    std::vector<std::uint8_t> again;
    std::vector<std::uint8_t> expected;
    VCC_CHECK(write_smf(read_back, again).ok);
    VCC_CHECK(write_smf(one_note_file(), expected).ok);
    VCC_CHECK(again == expected);
}

VCC_TEST_MAIN("midi file")
