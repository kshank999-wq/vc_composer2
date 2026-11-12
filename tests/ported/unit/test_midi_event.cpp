// Normalized MIDI event behaviour, including the raw-byte translation every
// host adapter depends on (spec 7).
#include "vcc/music/MidiEvent.h"

#include <cstddef>
#include <type_traits>

#include "vcc_test.h"

using namespace vcc::music;

VCC_TEST(event_is_pod_sized_for_realtime_queues)
{
    // The lock-free queues memcpy these across the audio-thread boundary.
    VCC_CHECK_EQ(sizeof(MidiEvent), static_cast<std::size_t>(16));
    VCC_CHECK(std::is_trivially_copyable<MidiEvent>::value);
}

VCC_TEST(constructors_mask_out_of_range_input)
{
    const MidiEvent note = make_note_on(200, 200, 200, 48000);
    VCC_CHECK_EQ(note.channel, static_cast<std::uint8_t>(200 & 0x0F));
    VCC_CHECK_EQ(note.data1, static_cast<std::uint8_t>(200 & 0x7F));
    VCC_CHECK_EQ(note.data2, static_cast<std::uint8_t>(200 & 0x7F));
    VCC_CHECK_EQ(note.sample_position, static_cast<std::int64_t>(48000));
    VCC_CHECK(is_valid(note));
}

VCC_TEST(default_event_is_invalid)
{
    const MidiEvent event;
    VCC_CHECK(!is_valid(event));
    VCC_CHECK_EQ(data_byte_count(MidiEventType::Invalid), 0);
}

VCC_TEST(validation_rejects_out_of_range_fields)
{
    MidiEvent event = make_note_on(0, 60, 100);
    event.data1 = 128;
    VCC_CHECK(!is_valid(event));

    MidiEvent bend = make_pitch_bend(0, 8192);
    bend.bend = 20000;
    VCC_CHECK(!is_valid(bend));
}

VCC_TEST(pitch_bend_keeps_14_bit_resolution)
{
    const MidiEvent bend = make_pitch_bend(3, 12345, 128);
    VCC_CHECK_EQ(bend.bend, static_cast<std::uint16_t>(12345));
    VCC_CHECK_EQ(bend.channel, static_cast<std::uint8_t>(3));
    VCC_CHECK(is_valid(bend));

    std::uint8_t bytes[3] = {0, 0, 0};
    VCC_CHECK_EQ(to_raw_bytes(bend, bytes, sizeof(bytes)), static_cast<std::size_t>(3));
    VCC_CHECK_EQ(bytes[0], static_cast<std::uint8_t>(0xE3));

    MidiEvent parsed;
    VCC_CHECK(from_raw_bytes(bytes, 3, 128, parsed));
    VCC_CHECK_EQ(parsed.bend, static_cast<std::uint16_t>(12345));
    VCC_CHECK_EQ(parsed.type, MidiEventType::PitchBend);
}

VCC_TEST(pitch_bend_clamps_above_range)
{
    const MidiEvent bend = make_pitch_bend(0, static_cast<std::uint16_t>(60000));
    VCC_CHECK_EQ(bend.bend, static_cast<std::uint16_t>(16383));
    VCC_CHECK(is_valid(bend));
}

VCC_TEST(round_trip_through_raw_bytes_preserves_every_supported_type)
{
    const MidiEvent events[] = {
        make_note_on(0, 60, 100, 10),
        make_note_off(15, 127, 64, 20),
        make_control_change(4, 7, 90, 30),
        make_program_change(9, 42, 40),
        make_channel_pressure(2, 55, 50),
        make_poly_pressure(7, 64, 77, 60),
        make_pitch_bend(1, 0, 70),
    };

    for (const MidiEvent& original : events) {
        std::uint8_t bytes[3] = {0, 0, 0};
        const std::size_t written = to_raw_bytes(original, bytes, sizeof(bytes));
        VCC_CHECK(written == 2 || written == 3);

        MidiEvent parsed;
        VCC_CHECK(from_raw_bytes(bytes, written, original.sample_position, parsed));
        VCC_CHECK_EQ(parsed.type, original.type);
        VCC_CHECK_EQ(parsed.channel, original.channel);
        VCC_CHECK_EQ(parsed.data1, original.data1);
        VCC_CHECK_EQ(parsed.sample_position, original.sample_position);
        if (written == 3 && original.type != MidiEventType::PitchBend)
            VCC_CHECK_EQ(parsed.data2, original.data2);
    }
}

VCC_TEST(one_byte_messages_encode_without_touching_data2)
{
    const MidiEvent program = make_program_change(0, 5);
    std::uint8_t bytes[3] = {0xFF, 0xFF, 0xFF};
    VCC_CHECK_EQ(to_raw_bytes(program, bytes, sizeof(bytes)), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(bytes[2], static_cast<std::uint8_t>(0xFF));  // Untouched.
}

VCC_TEST(parser_rejects_non_channel_voice_input)
{
    MidiEvent parsed;

    // System realtime (clock), system common (SysEx), and a bare data byte.
    const std::uint8_t clock[] = {0xF8};
    VCC_CHECK(!from_raw_bytes(clock, 1, 0, parsed));

    const std::uint8_t sysex[] = {0xF0, 0x7E, 0xF7};
    VCC_CHECK(!from_raw_bytes(sysex, 3, 0, parsed));

    const std::uint8_t running_status[] = {0x40, 0x50};
    VCC_CHECK(!from_raw_bytes(running_status, 2, 0, parsed));

    VCC_CHECK(!from_raw_bytes(nullptr, 3, 0, parsed));
}

VCC_TEST(parser_rejects_truncated_and_corrupt_messages)
{
    MidiEvent parsed;

    // Note-on needs two data bytes; only one is present.
    const std::uint8_t truncated[] = {0x90, 0x3C};
    VCC_CHECK(!from_raw_bytes(truncated, 2, 0, parsed));

    // A data byte with its high bit set is not a data byte.
    const std::uint8_t corrupt[] = {0x90, 0x3C, 0x80};
    VCC_CHECK(!from_raw_bytes(corrupt, 3, 0, parsed));
}

VCC_TEST(encoder_refuses_insufficient_capacity)
{
    const MidiEvent note = make_note_on(0, 60, 100);
    std::uint8_t bytes[2] = {0, 0};
    VCC_CHECK_EQ(to_raw_bytes(note, bytes, 2), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(to_raw_bytes(note, nullptr, 3), static_cast<std::size_t>(0));
}

VCC_TEST_MAIN("music/MidiEvent")
