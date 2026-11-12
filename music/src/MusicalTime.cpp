#include "vcc/music/MusicalTime.h"

#include <cmath>

namespace vcc::music {

const char* to_string(TransportState state) noexcept
{
    switch (state) {
        case TransportState::Stopped:   return "Stopped";
        case TransportState::Playing:   return "Playing";
        case TransportState::Recording: return "Recording";
        case TransportState::Unknown:   break;
    }
    return "Unknown";
}

bool TimeSignature::is_valid() const noexcept
{
    if (numerator == 0 || numerator > 128)
        return false;
    // Denominators are powers of two from 1 to 128.
    switch (denominator) {
        case 1: case 2: case 4: case 8: case 16: case 32: case 64: case 128:
            return true;
        default:
            return false;
    }
}

bool operator==(const TimeSignature& a, const TimeSignature& b) noexcept
{
    return a.numerator == b.numerator && a.denominator == b.denominator;
}

bool operator!=(const TimeSignature& a, const TimeSignature& b) noexcept
{
    return !(a == b);
}

bool MusicalPosition::is_playing() const noexcept
{
    return transport == TransportState::Playing || transport == TransportState::Recording;
}

double samples_to_ppq(std::int64_t samples, double sample_rate, double tempo_bpm) noexcept
{
    if (!(sample_rate > 0.0) || !(tempo_bpm > 0.0))
        return 0.0;
    const double seconds = static_cast<double>(samples) / sample_rate;
    return seconds * (tempo_bpm / 60.0);
}

std::int64_t ppq_to_samples(double ppq, double sample_rate, double tempo_bpm) noexcept
{
    if (!(sample_rate > 0.0) || !(tempo_bpm > 0.0) || !std::isfinite(ppq))
        return 0;
    const double seconds = ppq * (60.0 / tempo_bpm);
    return static_cast<std::int64_t>(std::llround(seconds * sample_rate));
}

}  // namespace vcc::music
