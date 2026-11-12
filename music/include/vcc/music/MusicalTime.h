// Copyright (c) VC Composer. All rights reserved.
//
// Normalized musical-time types (spec 5, 7). Whatever a host exposes - AAX
// tempo/meter/sample position, or another DAW's equivalent - is reduced to
// these structures before it reaches the bridge.
#pragma once

#include <cstdint>

namespace vcc::music {

/// Host transport run state, normalized across DAWs.
enum class TransportState : std::uint8_t {
    Unknown   = 0,
    Stopped   = 1,
    Playing   = 2,
    Recording = 3,
};

const char* to_string(TransportState state) noexcept;

/// Score ticks per quarter note: the scale of a `MidiEvent` in musical time.
///
/// The notation library counts in the same unit, and `NotationSync` asserts
/// that the two agree, because a tick that crosses the bridge is placed by
/// the plug-in against the host's quarter-note position and nothing on that
/// side can check the scale it was written in.
constexpr std::int32_t kTicksPerQuarter = 960;

struct TimeSignature {
    std::uint16_t numerator   = 4;
    std::uint16_t denominator = 4;

    bool is_valid() const noexcept;
};

bool operator==(const TimeSignature& a, const TimeSignature& b) noexcept;
bool operator!=(const TimeSignature& a, const TimeSignature& b) noexcept;

/// A snapshot of host musical position and tempo, as of the start of one
/// process block.
///
/// Every field carries a `*_valid` flag rather than a magic value: AAX (like
/// most hosts) does not guarantee that all of these are available in every
/// session state, and the plug-in must never present a guessed tempo to
/// VC Composer as though the host had reported it (spec 17).
struct MusicalPosition {
    /// Absolute sample position on the host timeline at the start of the block.
    std::int64_t sample_position = 0;
    bool sample_position_valid = false;

    /// Position in quarter notes from the session start.
    double ppq_position = 0.0;
    bool ppq_position_valid = false;

    double tempo_bpm = 0.0;
    bool tempo_valid = false;

    TimeSignature time_signature{};
    bool time_signature_valid = false;

    TransportState transport = TransportState::Unknown;

    bool is_looping = false;
    double loop_start_ppq = 0.0;
    double loop_end_ppq = 0.0;
    bool loop_range_valid = false;

    bool is_playing() const noexcept;
};

/// Converts samples to quarter notes at a fixed tempo. Only valid across a
/// span with no tempo change; the bridge uses it for within-block offsets,
/// never for long-range extrapolation.
double samples_to_ppq(std::int64_t samples, double sample_rate, double tempo_bpm) noexcept;

/// Inverse of `samples_to_ppq`, with the same constant-tempo caveat.
std::int64_t ppq_to_samples(double ppq, double sample_rate, double tempo_bpm) noexcept;

}  // namespace vcc::music
