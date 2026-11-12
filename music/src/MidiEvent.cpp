#include "vcc/music/MidiEvent.h"

#include <cstddef>

namespace vcc::music {
namespace {

constexpr std::uint8_t kMaxChannel = 15;
constexpr std::uint8_t kMaxDataByte = 127;
constexpr std::uint16_t kMaxBend = 16383;

MidiEvent make(MidiEventType type, std::uint8_t channel, std::uint8_t d1, std::uint8_t d2,
               std::int64_t position) noexcept
{
    MidiEvent e;
    e.sample_position = position;
    e.type = type;
    e.channel = static_cast<std::uint8_t>(channel & kMaxChannel);
    e.data1 = static_cast<std::uint8_t>(d1 & kMaxDataByte);
    e.data2 = static_cast<std::uint8_t>(d2 & kMaxDataByte);
    return e;
}

}  // namespace

int data_byte_count(MidiEventType type) noexcept
{
    switch (type) {
        case MidiEventType::ProgramChange:
        case MidiEventType::ChannelPressure:
            return 1;
        case MidiEventType::NoteOff:
        case MidiEventType::NoteOn:
        case MidiEventType::PolyPressure:
        case MidiEventType::ControlChange:
        case MidiEventType::PitchBend:
            return 2;
        case MidiEventType::Invalid:
            break;
    }
    return 0;
}

bool is_valid(const MidiEvent& event) noexcept
{
    if (data_byte_count(event.type) == 0)
        return false;
    if (event.channel > kMaxChannel)
        return false;
    if (event.data1 > kMaxDataByte || event.data2 > kMaxDataByte)
        return false;
    if (event.type == MidiEventType::PitchBend && event.bend > kMaxBend)
        return false;
    return true;
}

MidiEvent make_note_on(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity,
                       std::int64_t sample_position) noexcept
{
    return make(MidiEventType::NoteOn, channel, note, velocity, sample_position);
}

MidiEvent make_note_off(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity,
                        std::int64_t sample_position) noexcept
{
    return make(MidiEventType::NoteOff, channel, note, velocity, sample_position);
}

MidiEvent make_control_change(std::uint8_t channel, std::uint8_t controller, std::uint8_t value,
                              std::int64_t sample_position) noexcept
{
    return make(MidiEventType::ControlChange, channel, controller, value, sample_position);
}

MidiEvent make_program_change(std::uint8_t channel, std::uint8_t program,
                              std::int64_t sample_position) noexcept
{
    return make(MidiEventType::ProgramChange, channel, program, 0, sample_position);
}

MidiEvent make_pitch_bend(std::uint8_t channel, std::uint16_t bend_14bit,
                          std::int64_t sample_position) noexcept
{
    const std::uint16_t bend = bend_14bit > kMaxBend ? kMaxBend : bend_14bit;
    MidiEvent e = make(MidiEventType::PitchBend, channel,
                       static_cast<std::uint8_t>(bend & 0x7F),
                       static_cast<std::uint8_t>((bend >> 7) & 0x7F), sample_position);
    e.bend = bend;
    return e;
}

MidiEvent make_channel_pressure(std::uint8_t channel, std::uint8_t pressure,
                                std::int64_t sample_position) noexcept
{
    return make(MidiEventType::ChannelPressure, channel, pressure, 0, sample_position);
}

MidiEvent make_poly_pressure(std::uint8_t channel, std::uint8_t note, std::uint8_t pressure,
                             std::int64_t sample_position) noexcept
{
    return make(MidiEventType::PolyPressure, channel, note, pressure, sample_position);
}

bool from_raw_bytes(const std::uint8_t* bytes, std::size_t length, std::int64_t sample_position,
                    MidiEvent& out) noexcept
{
    if (bytes == nullptr || length == 0)
        return false;

    const std::uint8_t status = bytes[0];
    if (status < 0x80 || status >= 0xF0)
        return false;  // Running status, data byte, or system/realtime message.

    const auto type = static_cast<MidiEventType>((status >> 4) & 0x0F);
    const int expected = data_byte_count(type);
    if (expected == 0 || length < static_cast<std::size_t>(expected) + 1)
        return false;

    const std::uint8_t d1 = bytes[1];
    const std::uint8_t d2 = expected == 2 ? bytes[2] : 0;
    if (d1 > kMaxDataByte || d2 > kMaxDataByte)
        return false;  // A set high bit means we are not looking at data bytes.

    out = make(type, static_cast<std::uint8_t>(status & 0x0F), d1, d2, sample_position);
    if (type == MidiEventType::PitchBend)
        out.bend = static_cast<std::uint16_t>(d1 | (static_cast<std::uint16_t>(d2) << 7));
    return true;
}

std::size_t to_raw_bytes(const MidiEvent& event, std::uint8_t* out, std::size_t capacity) noexcept
{
    if (out == nullptr || !is_valid(event))
        return 0;

    const int data_bytes = data_byte_count(event.type);
    const std::size_t total = static_cast<std::size_t>(data_bytes) + 1;
    if (capacity < total)
        return 0;

    out[0] = static_cast<std::uint8_t>((static_cast<std::uint8_t>(event.type) << 4) | event.channel);
    if (event.type == MidiEventType::PitchBend) {
        out[1] = static_cast<std::uint8_t>(event.bend & 0x7F);
        out[2] = static_cast<std::uint8_t>((event.bend >> 7) & 0x7F);
    } else {
        out[1] = event.data1;
        if (data_bytes == 2)
            out[2] = event.data2;
    }
    return total;
}

const char* to_string(MidiEventType type) noexcept
{
    switch (type) {
        case MidiEventType::NoteOff:         return "NoteOff";
        case MidiEventType::NoteOn:          return "NoteOn";
        case MidiEventType::PolyPressure:    return "PolyPressure";
        case MidiEventType::ControlChange:   return "CC";
        case MidiEventType::ProgramChange:   return "ProgramChange";
        case MidiEventType::ChannelPressure: return "ChannelPressure";
        case MidiEventType::PitchBend:       return "PitchBend";
        case MidiEventType::Invalid:         break;
    }
    return "Invalid";
}

}  // namespace vcc::music
