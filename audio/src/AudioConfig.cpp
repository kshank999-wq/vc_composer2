#include "vcc/audio/AudioConfig.h"

namespace vcc::audio {

const char* to_string(RenderMode mode) noexcept
{
    return mode == RenderMode::Offline ? "Offline" : "Realtime";
}

bool AudioConfig::is_valid() const noexcept
{
    if (!(sample_rate >= kMinSampleRate && sample_rate <= kMaxSampleRate))
        return false;
    if (max_block_size <= 0 || max_block_size > kMaxSupportedBlockSize)
        return false;
    if (input_channels < 0 || output_channels < 0)
        return false;
    return true;
}

bool operator==(const AudioConfig& a, const AudioConfig& b) noexcept
{
    return a.sample_rate == b.sample_rate
        && a.max_block_size == b.max_block_size
        && a.input_channels == b.input_channels
        && a.output_channels == b.output_channels
        && a.render_mode == b.render_mode;
}

bool operator!=(const AudioConfig& a, const AudioConfig& b) noexcept
{
    return !(a == b);
}

}  // namespace vcc::audio
