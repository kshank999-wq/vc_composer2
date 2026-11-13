#include "vcc/music/MidiFile.h"

#include "vcc/core/Files.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace vcc::music {
namespace {

/// Microseconds per quarter note is a 24-bit field, so the tempo range the
/// format can express is bounded at both ends.
constexpr std::int64_t kMinMicrosecondsPerQuarter = 1;
constexpr std::int64_t kMaxMicrosecondsPerQuarter = 0xFFFFFF;

/// MIDI clocks per metronome click. 24 is one quarter note, which is the
/// value every sequencer writes and the only one with a settled meaning.
constexpr std::uint8_t kClocksPerClick = 24;

/// Thirty-second notes per 24 MIDI clocks. 8 is the standard value; it means
/// "a quarter note is a quarter note".
constexpr std::uint8_t kThirtySecondsPerClick = 8;

void append_u16(std::uint16_t value, std::vector<std::uint8_t>& out)
{
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void append_u32(std::uint32_t value, std::vector<std::uint8_t>& out)
{
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void append_chunk_id(const char* id, std::vector<std::uint8_t>& out)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(id[i]));
}

/// Where an event sits among others at the same tick.
///
/// The order within a tick is not cosmetic. A note-off must precede a
/// note-on of the same pitch, or a repeated note is cut off by its own
/// predecessor's release instead of re-articulating. A tempo or meter change
/// must precede the notes it governs. Everything else keeps the order the
/// caller supplied, which is how a keyswitch that the renderer deliberately
/// placed before a note-on stays there.
enum class Rank : int {
    Meta = 0,
    NoteOff = 1,
    Controller = 2,
    NoteOn = 3,
};

Rank rank_of(const MidiEvent& event)
{
    switch (event.type) {
        case MidiEventType::NoteOff:
            return Rank::NoteOff;
        case MidiEventType::NoteOn:
            // A note-on with velocity 0 is a note-off in disguise, and every
            // reader treats it as one - so it has to be ordered as one too.
            return event.data2 == 0 ? Rank::NoteOff : Rank::NoteOn;
        default:
            break;
    }
    return Rank::Controller;
}

/// One event, already turned into the bytes that follow its delta time.
struct Item {
    std::int64_t tick = 0;
    Rank rank = Rank::Controller;
    std::size_t order = 0;  ///< Keeps a sort stable across ties in tick and rank.
    std::vector<std::uint8_t> bytes;
};

void append_meta_bytes(const SmfMetaEvent& meta, std::vector<std::uint8_t>& out)
{
    out.push_back(0xFF);
    out.push_back(meta.type);
    // The payload length is itself a variable-length quantity. A payload
    // this long cannot occur from the helpers above, but a caller can build
    // a meta event by hand.
    append_variable_length(static_cast<std::int64_t>(meta.data.size()), out);
    out.insert(out.end(), meta.data.begin(), meta.data.end());
}

}  // namespace

// ---------------------------------------------------------------------------
// Variable-length quantities
// ---------------------------------------------------------------------------

bool append_variable_length(std::int64_t value, std::vector<std::uint8_t>& out)
{
    if (value < 0 || value > kSmfMaxDeltaTicks)
        return false;

    // Built most-significant group first into a small buffer, because the
    // encoding is big-endian and the low group is the one that terminates.
    std::uint8_t buffer[4] = {0, 0, 0, 0};
    int count = 0;
    auto remaining = static_cast<std::uint32_t>(value);
    do {
        buffer[count++] = static_cast<std::uint8_t>(remaining & 0x7F);
        remaining >>= 7;
    } while (remaining != 0 && count < 4);

    for (int i = count - 1; i >= 0; --i) {
        const std::uint8_t continuation = i > 0 ? 0x80 : 0x00;
        out.push_back(static_cast<std::uint8_t>(buffer[i] | continuation));
    }
    return true;
}

bool read_variable_length(const std::uint8_t* bytes, std::size_t length, std::size_t& at,
                          std::int64_t& out)
{
    if (bytes == nullptr)
        return false;

    std::int64_t value = 0;
    for (int consumed = 0; consumed < 4; ++consumed) {
        if (at >= length)
            return false;
        const std::uint8_t byte = bytes[at++];
        value = (value << 7) | static_cast<std::int64_t>(byte & 0x7F);
        if ((byte & 0x80) == 0) {
            out = value;
            return true;
        }
    }
    // A fifth continuation byte: not a quantity this format can hold.
    return false;
}

int smf_denominator_exponent(int denominator) noexcept
{
    if (denominator <= 0)
        return -1;
    int exponent = 0;
    int value = denominator;
    while ((value & 1) == 0) {
        value >>= 1;
        ++exponent;
    }
    // Only a power of two reduces to 1 by halving.
    return value == 1 ? exponent : -1;
}

// ---------------------------------------------------------------------------
// Meta-event construction
// ---------------------------------------------------------------------------

SmfMetaEvent smf_tempo(std::int64_t tick, double quarter_notes_per_minute, bool& ok)
{
    SmfMetaEvent meta;
    meta.tick = tick;
    meta.type = smf_meta::kTempo;
    ok = false;

    if (!(quarter_notes_per_minute > 0.0) || !std::isfinite(quarter_notes_per_minute))
        return meta;

    const double microseconds = 60000000.0 / quarter_notes_per_minute;
    const auto rounded = static_cast<std::int64_t>(std::llround(microseconds));
    if (rounded < kMinMicrosecondsPerQuarter || rounded > kMaxMicrosecondsPerQuarter)
        return meta;

    meta.data = {
        static_cast<std::uint8_t>((rounded >> 16) & 0xFF),
        static_cast<std::uint8_t>((rounded >> 8) & 0xFF),
        static_cast<std::uint8_t>(rounded & 0xFF),
    };
    ok = true;
    return meta;
}

SmfMetaEvent smf_tempo(std::int64_t tick, double quarter_notes_per_minute)
{
    bool ignored = false;
    return smf_tempo(tick, quarter_notes_per_minute, ignored);
}

SmfMetaEvent smf_time_signature(std::int64_t tick, const TimeSignature& time_signature,
                                bool& ok)
{
    SmfMetaEvent meta;
    meta.tick = tick;
    meta.type = smf_meta::kTimeSignature;
    ok = false;

    if (!time_signature.is_valid() || time_signature.numerator > 0xFF)
        return meta;

    const int exponent = smf_denominator_exponent(static_cast<int>(time_signature.denominator));
    if (exponent < 0 || exponent > 0xFF)
        return meta;

    meta.data = {
        static_cast<std::uint8_t>(time_signature.numerator),
        static_cast<std::uint8_t>(exponent),
        kClocksPerClick,
        kThirtySecondsPerClick,
    };
    ok = true;
    return meta;
}

SmfMetaEvent smf_time_signature(std::int64_t tick, const TimeSignature& time_signature)
{
    bool ignored = false;
    return smf_time_signature(tick, time_signature, ignored);
}

SmfMetaEvent smf_key_signature(std::int64_t tick, int fifths, bool is_minor, bool& ok)
{
    SmfMetaEvent meta;
    meta.tick = tick;
    meta.type = smf_meta::kKeySignature;
    ok = false;

    if (fifths < -7 || fifths > 7)
        return meta;

    meta.data = {
        static_cast<std::uint8_t>(static_cast<std::int8_t>(fifths)),
        static_cast<std::uint8_t>(is_minor ? 1 : 0),
    };
    ok = true;
    return meta;
}

SmfMetaEvent smf_key_signature(std::int64_t tick, int fifths, bool is_minor)
{
    bool ignored = false;
    return smf_key_signature(tick, fifths, is_minor, ignored);
}

SmfMetaEvent smf_text(std::int64_t tick, std::uint8_t type, const std::string& text)
{
    SmfMetaEvent meta;
    meta.tick = tick;
    meta.type = type;
    meta.data.assign(text.begin(), text.end());
    return meta;
}

SmfMetaEvent smf_track_name(const std::string& name)
{
    return smf_text(0, smf_meta::kTrackName, name);
}

SmfMetaEvent smf_marker(std::int64_t tick, const std::string& text)
{
    return smf_text(tick, smf_meta::kMarker, text);
}

bool is_valid(const SmfMetaEvent& meta) noexcept
{
    if (meta.tick < 0)
        return false;

    switch (meta.type) {
        case smf_meta::kTempo:         return meta.data.size() == 3;
        case smf_meta::kTimeSignature: return meta.data.size() == 4;
        case smf_meta::kKeySignature:  return meta.data.size() == 2;
        case smf_meta::kEndOfTrack:    return meta.data.empty();
        default:                       break;
    }
    // An open-ended type with any payload, a zero-length text included.
    return true;
}

std::size_t SmfFile::event_count() const noexcept
{
    std::size_t count = 0;
    for (const SmfTrack& track : tracks)
        count += track.events.size() + track.meta.size();
    return count;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

namespace {

SmfWriteResult refuse(std::string error)
{
    SmfWriteResult result;
    result.ok = false;
    result.error = std::move(error);
    return result;
}

/// Serializes one track's data (without the chunk header), reporting what it
/// left out.
bool build_track_data(const SmfTrack& track, std::size_t track_index,
                      std::vector<std::uint8_t>& out, SmfWriteResult& result)
{
    std::vector<Item> items;
    items.reserve(track.events.size() + track.meta.size() + 1);

    std::size_t order = 0;

    // The name first, so it is the first thing a reader meets and shows as
    // the track's name.
    if (!track.name.empty()) {
        Item item;
        item.tick = 0;
        item.rank = Rank::Meta;
        item.order = order++;
        append_meta_bytes(smf_track_name(track.name), item.bytes);
        items.push_back(std::move(item));
    }

    for (const SmfMetaEvent& meta : track.meta) {
        if (meta.type == smf_meta::kEndOfTrack) {
            // The writer owns the end-of-track event: writing a second one
            // would put bytes after the end of the track, which is a
            // corrupt file rather than a data problem.
            result.error = "track " + std::to_string(track_index)
                           + " carries its own end-of-track event";
            return false;
        }
        if (meta.tick < 0) {
            result.error = "track " + std::to_string(track_index)
                           + " has a meta event before the start of the file";
            return false;
        }
        if (!is_valid(meta)) {
            ++result.dropped_events;
            continue;
        }

        Item item;
        item.tick = meta.tick;
        item.rank = Rank::Meta;
        item.order = order++;
        append_meta_bytes(meta, item.bytes);
        items.push_back(std::move(item));
    }

    for (const SmfEvent& event : track.events) {
        if (event.tick < 0) {
            result.error = "track " + std::to_string(track_index)
                           + " has an event before the start of the file";
            return false;
        }

        std::uint8_t bytes[3] = {0, 0, 0};
        const std::size_t length = to_raw_bytes(event.event, bytes, sizeof(bytes));
        if (length == 0) {
            // Not a MIDI message this representation can express - a note
            // out of range, an uninitialized event. Counted, so a caller
            // can say how many rather than shipping a short file.
            ++result.dropped_events;
            continue;
        }

        Item item;
        item.tick = event.tick;
        item.rank = rank_of(event.event);
        item.order = order++;
        item.bytes.assign(bytes, bytes + length);
        items.push_back(std::move(item));
    }

    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.tick != b.tick)
            return a.tick < b.tick;
        if (a.rank != b.rank)
            return static_cast<int>(a.rank) < static_cast<int>(b.rank);
        return a.order < b.order;
    });

    // Running status is deliberately not used. It would save a byte on most
    // events, which is worth nothing here, and it is the part of this format
    // that readers most often get wrong - so every event carries its own
    // status byte.
    std::int64_t previous_tick = 0;
    for (const Item& item : items) {
        const std::int64_t delta = item.tick - previous_tick;
        if (!append_variable_length(delta, out)) {
            result.error = "track " + std::to_string(track_index) + " has a gap of "
                           + std::to_string(delta)
                           + " ticks, which is longer than the format can express";
            return false;
        }
        out.insert(out.end(), item.bytes.begin(), item.bytes.end());
        previous_tick = item.tick;
        ++result.event_count;
    }

    // End of track, at the last event's position. Required: a track chunk
    // without one is malformed, even when it is empty.
    append_variable_length(0, out);
    append_meta_bytes(SmfMetaEvent{previous_tick, smf_meta::kEndOfTrack, {}}, out);
    ++result.event_count;
    return true;
}

}  // namespace

SmfWriteResult write_smf(const SmfFile& file, std::vector<std::uint8_t>& out)
{
    out.clear();

    if (file.ticks_per_quarter == 0 || file.ticks_per_quarter > kSmfMaxDivision) {
        return refuse("a MIDI file's division must be 1 to "
                      + std::to_string(kSmfMaxDivision) + " ticks per quarter note, not "
                      + std::to_string(file.ticks_per_quarter));
    }
    if (file.tracks.empty())
        return refuse("a MIDI file needs at least one track");
    if (file.tracks.size() > 0xFFFF) {
        return refuse("a MIDI file can hold at most 65535 tracks, not "
                      + std::to_string(file.tracks.size()));
    }
    if (file.format == SmfFormat::SingleTrack && file.tracks.size() != 1) {
        // Format 0 means one track by definition. Writing several under that
        // header produces a file whose contents contradict it.
        return refuse("a format 0 MIDI file holds exactly one track, not "
                      + std::to_string(file.tracks.size()));
    }

    SmfWriteResult result;

    append_chunk_id("MThd", out);
    append_u32(6, out);
    append_u16(static_cast<std::uint16_t>(file.format), out);
    append_u16(static_cast<std::uint16_t>(file.tracks.size()), out);
    append_u16(file.ticks_per_quarter, out);

    for (std::size_t index = 0; index < file.tracks.size(); ++index) {
        std::vector<std::uint8_t> data;
        if (!build_track_data(file.tracks[index], index, data, result)) {
            out.clear();
            result.ok = false;
            return result;
        }

        append_chunk_id("MTrk", out);
        append_u32(static_cast<std::uint32_t>(data.size()), out);
        out.insert(out.end(), data.begin(), data.end());
        ++result.track_count;
    }

    result.ok = true;
    result.byte_count = out.size();
    return result;
}

namespace {

struct Reader {
    const std::uint8_t* bytes = nullptr;
    std::size_t size = 0;
    std::size_t at = 0;

    bool remaining(std::size_t count) const { return at + count <= size; }

    std::uint32_t u32()
    {
        const std::uint32_t value = (static_cast<std::uint32_t>(bytes[at]) << 24)
                                    | (static_cast<std::uint32_t>(bytes[at + 1]) << 16)
                                    | (static_cast<std::uint32_t>(bytes[at + 2]) << 8)
                                    | static_cast<std::uint32_t>(bytes[at + 3]);
        at += 4;
        return value;
    }

    std::uint16_t u16()
    {
        const std::uint16_t value =
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[at]) << 8) | bytes[at + 1]);
        at += 2;
        return value;
    }

    bool chunk_is(const char* id) const
    {
        for (int i = 0; i < 4; ++i) {
            if (bytes[at + static_cast<std::size_t>(i)] != static_cast<std::uint8_t>(id[i]))
                return false;
        }
        return true;
    }
};

SmfReadResult refuse_read(std::string error)
{
    SmfReadResult result;
    result.error = std::move(error);
    return result;
}

/// Builds a MidiEvent from a status byte and its data. Returns false for a
/// status this representation has no type for.
bool event_from_status(std::uint8_t status, std::uint8_t data1, std::uint8_t data2,
                       MidiEvent& out)
{
    const auto type = static_cast<MidiEventType>(status >> 4);
    const std::uint8_t channel = static_cast<std::uint8_t>(status & 0x0F);
    if (data_byte_count(type) == 0)
        return false;

    switch (type) {
        case MidiEventType::PitchBend:
            out = make_pitch_bend(channel, static_cast<std::uint16_t>(
                                               (static_cast<std::uint16_t>(data2 & 0x7F) << 7)
                                               | (data1 & 0x7F)));
            return true;
        case MidiEventType::ProgramChange:
            out = make_program_change(channel, data1);
            return true;
        case MidiEventType::ChannelPressure:
            out = make_channel_pressure(channel, data1);
            return true;
        case MidiEventType::NoteOn:
            out = make_note_on(channel, data1, data2);
            return true;
        case MidiEventType::NoteOff:
            out = make_note_off(channel, data1, data2);
            return true;
        case MidiEventType::PolyPressure:
            out = make_poly_pressure(channel, data1, data2);
            return true;
        case MidiEventType::ControlChange:
            out = make_control_change(channel, data1, data2);
            return true;
        case MidiEventType::Invalid:
            break;
    }
    return false;
}

}  // namespace

SmfReadResult read_smf(const std::vector<std::uint8_t>& bytes, SmfFile& out)
{
    out = SmfFile{};
    out.tracks.clear();

    Reader reader{bytes.data(), bytes.size(), 0};
    if (!reader.remaining(14) || !reader.chunk_is("MThd"))
        return refuse_read("that is not a standard MIDI file");

    reader.at += 4;
    const std::uint32_t header_length = reader.u32();
    if (header_length < 6 || !reader.remaining(header_length))
        return refuse_read("the file's header is not a MIDI header");

    const std::size_t header_end = reader.at + header_length;
    const std::uint16_t format = reader.u16();
    const std::uint16_t declared_tracks = reader.u16();
    const std::uint16_t division = reader.u16();

    // A longer header is allowed and its extra bytes are to be ignored.
    reader.at = header_end;

    SmfReadResult result;
    result.format = static_cast<int>(format);

    if (format > 1) {
        // Format 2's tracks are independent sequences rather than parts of
        // one piece, so reading them as simultaneous parts would be wrong in
        // a way nothing later could detect.
        return refuse_read("this is a format " + std::to_string(format)
                           + " MIDI file, whose tracks are separate pieces rather than parts "
                             "of one");
    }
    if ((division & 0x8000) != 0) {
        // SMPTE division measures in frames, not beats. There is no musical
        // time in it to import.
        return refuse_read("this file measures time in SMPTE frames rather than beats");
    }
    if (division == 0)
        return refuse_read("this file says there are no ticks in a quarter note");

    out.format = format == 0 ? SmfFormat::SingleTrack : SmfFormat::MultiTrack;
    out.ticks_per_quarter = division;

    while (reader.at + 8 <= reader.size) {
        const bool is_track = reader.chunk_is("MTrk");
        reader.at += 4;
        const std::uint32_t length = reader.u32();

        if (!reader.remaining(length))
            return refuse_read("a chunk in this file runs past the end of it");

        const std::size_t end = reader.at + length;
        if (!is_track) {
            // Unknown chunks are to be ignored, which is how the format
            // stays extensible - but a caller is told there was one.
            ++result.skipped_events;
            reader.at = end;
            continue;
        }

        SmfTrack track;
        std::int64_t tick = 0;
        std::uint8_t running_status = 0;
        bool finished = false;

        while (reader.at < end && !finished) {
            std::int64_t delta = 0;
            if (!read_variable_length(reader.bytes, end, reader.at, delta))
                return refuse_read("a delta time in this file is malformed");
            tick += delta;

            if (reader.at >= end)
                return refuse_read("a track in this file ends in the middle of an event");

            std::uint8_t status = reader.bytes[reader.at];
            if ((status & 0x80) != 0) {
                ++reader.at;
                if (status < 0xF0)
                    running_status = status;
            } else {
                // Running status: the status byte is the previous one, and
                // what follows is data. This writer never produces it; most
                // files do.
                if (running_status == 0)
                    return refuse_read("this file begins a track with running status");
                status = running_status;
            }

            if (status == 0xFF) {
                if (reader.at >= end)
                    return refuse_read("a meta event in this file has no type");
                const std::uint8_t meta_type = reader.bytes[reader.at++];

                std::int64_t payload = 0;
                if (!read_variable_length(reader.bytes, end, reader.at, payload))
                    return refuse_read("a meta event's length in this file is malformed");
                if (!reader.remaining(static_cast<std::size_t>(payload))
                    || reader.at + static_cast<std::size_t>(payload) > end) {
                    return refuse_read("a meta event in this file runs past its track");
                }

                SmfMetaEvent meta;
                meta.tick = tick;
                meta.type = meta_type;
                meta.data.assign(reader.bytes + reader.at,
                                 reader.bytes + reader.at + static_cast<std::size_t>(payload));
                reader.at += static_cast<std::size_t>(payload);

                if (meta_type == smf_meta::kEndOfTrack) {
                    // The writer owns it, so it is consumed rather than
                    // kept: a track that carried its own would be refused
                    // on the way back out.
                    finished = true;
                    ++result.event_count;
                    continue;
                }

                // The name is lifted into the track, so reading and writing
                // a file this library produced gives the same bytes back.
                if (meta_type == smf_meta::kTrackName && tick == 0 && track.name.empty()
                    && track.events.empty() && track.meta.empty()) {
                    track.name.assign(meta.data.begin(), meta.data.end());
                } else {
                    track.meta.push_back(std::move(meta));
                }
                ++result.event_count;
                continue;
            }

            if (status >= 0xF0) {
                // System exclusive, and the two escapes. Length-prefixed, so
                // it can be stepped over exactly - but there is nowhere here
                // to keep it.
                std::int64_t payload = 0;
                if (!read_variable_length(reader.bytes, end, reader.at, payload))
                    return refuse_read("a system-exclusive block's length is malformed");
                if (reader.at + static_cast<std::size_t>(payload) > end)
                    return refuse_read("a system-exclusive block runs past its track");
                reader.at += static_cast<std::size_t>(payload);
                ++result.skipped_events;
                continue;
            }

            const auto type = static_cast<MidiEventType>(status >> 4);
            const std::size_t data_bytes =
                static_cast<std::size_t>(data_byte_count(type));
            if (data_bytes == 0)
                return refuse_read("this file carries a message this library cannot read");
            if (reader.at + data_bytes > end)
                return refuse_read("a channel message in this file is truncated");

            const std::uint8_t data1 = reader.bytes[reader.at++];
            const std::uint8_t data2 =
                data_bytes == 2 ? reader.bytes[reader.at++] : static_cast<std::uint8_t>(0);

            MidiEvent event;
            if (!event_from_status(status, data1, data2, event)) {
                ++result.skipped_events;
                continue;
            }
            track.events.push_back(SmfEvent{tick, event});
            ++result.event_count;
        }

        // A track whose bytes end without an end-of-track event is still a
        // track; the format requires one, and refusing a file over a missing
        // terminator would refuse music that is otherwise intact.
        reader.at = end;
        out.tracks.push_back(std::move(track));
    }

    if (out.tracks.empty())
        return refuse_read("there are no tracks in this file");

    if (declared_tracks != out.tracks.size()) {
        // Not fatal: what is there is what is there. Counted so a caller can
        // say the file disagrees with itself.
        ++result.skipped_events;
    }

    result.ok = true;
    result.byte_count = bytes.size();
    result.track_count = out.tracks.size();
    return result;
}

SmfReadResult read_smf_file(const std::string& path, SmfFile& out)
{
    if (path.empty())
        return refuse_read("no MIDI file path was given");

    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file)
        return refuse_read("could not open that MIDI file");

    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),
                                          std::istreambuf_iterator<char>()};
    return read_smf(bytes, out);
}

SmfWriteResult write_smf_file(const SmfFile& file, const std::string& path)
{
    if (path.empty())
        return refuse("no MIDI file path was given");

    // Serialized first, so a refusal never leaves a truncated file behind
    // for Pro Tools to import.
    std::vector<std::uint8_t> bytes;
    SmfWriteResult result = write_smf(file, bytes);
    if (!result.ok)
        return result;

    // Through a temporary beside the target. Exporting over a file Pro Tools
    // has already imported once is an ordinary thing to do, and a failed
    // write used to destroy that file before replacing it. The path stays
    // out of the error, which reaches logs (spec 01 sections 9, 10).
    const files::FileWriteResult written = files::write_file_atomically(
        path, std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));

    result.ok = written.ok;
    if (!written.ok)
        result.error = written.error;
    return result;
}

}  // namespace vcc::music
