// Copyright (c) VC Composer. All rights reserved.
//
// Audio/host configuration types (spec 8, 11). Host-agnostic by design.
#pragma once

#include <cstdint>

namespace vcc::audio {

/// Sample rates the bridge accepts. The authoritative list of rates a given
/// Pro Tools version supports comes from Avid's documentation (spec 17); this
/// set is the superset the transport layer will carry, and the host adapter
/// rejects anything outside it rather than silently mis-timing MIDI.
constexpr double kMinSampleRate = 44100.0;
constexpr double kMaxSampleRate = 192000.0;

/// Upper bound on a single process block. Real-time buffers are preallocated
/// to this size at `prepare` time, so it must cover the largest block a host
/// can request, including offline/bounce passes which commonly use much
/// larger blocks than the live buffer size.
constexpr std::int32_t kMaxSupportedBlockSize = 8192;

/// Reason a block is being processed, so VC Composer can distinguish a live
/// pass from a bounce (spec 5: offline/render handling).
enum class RenderMode : std::uint8_t {
    Realtime = 0,
    Offline  = 1,  ///< Bounce / offline render / AudioSuite-style pass.
};

const char* to_string(RenderMode mode) noexcept;

/// Everything the plug-in core needs to preallocate and to time events.
/// Populated on the non-real-time thread before processing starts and
/// whenever the host changes configuration (spec 5).
struct AudioConfig {
    double       sample_rate = 0.0;
    std::int32_t max_block_size = 0;
    std::int32_t input_channels = 0;
    std::int32_t output_channels = 0;
    RenderMode   render_mode = RenderMode::Realtime;

    /// True once the host has reported a usable configuration.
    bool is_valid() const noexcept;
};

bool operator==(const AudioConfig& a, const AudioConfig& b) noexcept;
bool operator!=(const AudioConfig& a, const AudioConfig& b) noexcept;

}  // namespace vcc::audio
