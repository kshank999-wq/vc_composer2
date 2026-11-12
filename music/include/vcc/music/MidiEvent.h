// Copyright (c) VC Composer. All rights reserved.
//
// Normalized MIDI event representation (spec 7). This type is deliberately
// independent of AAX, JUCE and any host SDK: it is the only MIDI
// representation that crosses the bridge, and host adapters translate into
// and out of it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vcc::music {

/// Channel-voice message kinds required by the VC Composer workflow (spec 7).
/// Values match the MIDI 1.0 status nibble so translation is a shift, not a
/// lookup table.
enum class MidiEventType : std::uint8_t {
    Invalid        = 0x00,
    NoteOff        = 0x08,
    NoteOn         = 0x09,
    PolyPressure   = 0x0A,  ///< Polyphonic (per-note) aftertouch.
    ControlChange  = 0x0B,
    ProgramChange  = 0x0C,
    ChannelPressure= 0x0D,  ///< Channel aftertouch.
    PitchBend      = 0x0E,
};

/// Sentinel for "this event has no known timeline position".
constexpr std::int64_t kUnknownSamplePosition = INT64_MIN;

/// `MidiEvent::flags`: the position is a score tick, not a host sample.
///
/// The tick is `kTicksPerQuarter` per quarter note from the session start,
/// the same scale as the host's ppq position. An event in musical time is
/// placed by whoever plays it, at the tempo in force *when it plays*, which
/// is what lets a phrase sent ahead of the playhead follow a ritardando the
/// host reaches after the phrase was sent (docs/aax/12-notation-to-host.md).
constexpr std::uint16_t kMidiEventMusicalTime = 1u << 0;

/// A single normalized musical event.
///
/// Trivially copyable POD: it is memcpy-ed into lock-free queues from the
/// audio thread, so it must never gain a constructor that allocates, a
/// virtual table, or a non-trivial member.
struct MidiEvent {
    /// Sample offset. Within a process block this is relative to the start of
    /// the block (0 .. block_size-1); once timestamped by the bridge it is an
    /// absolute host timeline position. `kUnknownSamplePosition` means the
    /// producer could not determine a position.
    ///
    /// With `kMidiEventMusicalTime` in `flags` it is a score tick instead,
    /// and the host sample is not known until the event is played.
    std::int64_t sample_position = 0;

    MidiEventType type    = MidiEventType::Invalid;
    std::uint8_t  channel = 0;  ///< 0-15 (wire form; displayed as 1-16).
    std::uint8_t  data1   = 0;  ///< Note number / CC number / program.
    std::uint8_t  data2   = 0;  ///< Velocity / CC value / pressure.

    /// 14-bit pitch bend, 0..16383 with 8192 as centre. Only meaningful when
    /// `type == PitchBend`; kept as a separate field so callers never have to
    /// reassemble it from data1/data2 by hand.
    std::uint16_t bend = 8192;

    /// `kMidiEventMusicalTime`, or nothing. Was padding: the struct stays at
    /// 16 bytes.
    std::uint16_t flags = 0;
};

static_assert(sizeof(MidiEvent) == 16, "MidiEvent must stay compact and padding-free");

/// True when `sample_position` is a score tick rather than a host sample.
constexpr bool is_in_musical_time(const MidiEvent& event) noexcept
{
    return (event.flags & kMidiEventMusicalTime) != 0;
}

/// True when every field is within its MIDI-legal range. Messages arriving
/// over the bridge are validated with this before they are handed to a host
/// (spec 8 / acceptance: malformed-message handling).
bool is_valid(const MidiEvent& event) noexcept;

/// Number of MIDI 1.0 data bytes a message of this type carries (1 or 2).
/// Returns 0 for `Invalid`.
int data_byte_count(MidiEventType type) noexcept;

// ---------------------------------------------------------------------------
// Constructors. Free functions rather than member constructors so MidiEvent
// stays an aggregate.
// ---------------------------------------------------------------------------

MidiEvent make_note_on(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity,
                       std::int64_t sample_position = 0) noexcept;
MidiEvent make_note_off(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity = 0,
                        std::int64_t sample_position = 0) noexcept;
MidiEvent make_control_change(std::uint8_t channel, std::uint8_t controller, std::uint8_t value,
                              std::int64_t sample_position = 0) noexcept;
MidiEvent make_program_change(std::uint8_t channel, std::uint8_t program,
                              std::int64_t sample_position = 0) noexcept;
MidiEvent make_pitch_bend(std::uint8_t channel, std::uint16_t bend_14bit,
                          std::int64_t sample_position = 0) noexcept;
MidiEvent make_channel_pressure(std::uint8_t channel, std::uint8_t pressure,
                                std::int64_t sample_position = 0) noexcept;
MidiEvent make_poly_pressure(std::uint8_t channel, std::uint8_t note, std::uint8_t pressure,
                             std::int64_t sample_position = 0) noexcept;

// ---------------------------------------------------------------------------
// Raw MIDI 1.0 byte translation. Host adapters (AAX via JUCE, or any future
// DAW adapter) use these two functions and nothing else.
// ---------------------------------------------------------------------------

/// Parses a channel-voice message. `length` must be the true message length.
/// Returns false for system/realtime messages, running status, SysEx, and
/// anything malformed - callers drop those rather than guessing.
bool from_raw_bytes(const std::uint8_t* bytes, std::size_t length,
                    std::int64_t sample_position, MidiEvent& out) noexcept;

/// Serializes to MIDI 1.0 bytes. `out` must have room for 3 bytes; returns the
/// number written (0 if the event is invalid).
std::size_t to_raw_bytes(const MidiEvent& event, std::uint8_t* out, std::size_t capacity) noexcept;

/// Stable short name for diagnostics and logs ("NoteOn", "CC", ...).
const char* to_string(MidiEventType type) noexcept;

}  // namespace vcc::music
