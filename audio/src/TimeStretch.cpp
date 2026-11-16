#include "vcc/audio/TimeStretch.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace vcc::audio {

Channels reversed(const Channels& in)
{
    Channels out = in;
    for (auto& c : out) std::reverse(c.begin(), c.end());
    return out;
}

Channels time_stretch(const Channels& in, double ratio, double sample_rate)
{
    if (in.empty() || in.front().empty()) return in;
    ratio = std::clamp(ratio, 0.25, 4.0);
    const std::size_t channels = in.size();
    const std::size_t length = in.front().size();
    const auto out_length = static_cast<std::size_t>(std::llround(static_cast<double>(length) * ratio));
    if (std::abs(ratio - 1.0) < 1e-6) return in;

    const auto frame = static_cast<std::size_t>(std::max(256.0, std::round(0.04 * sample_rate)) / 2) * 2;
    const std::size_t hop_out = frame / 2;
    const double hop_in = static_cast<double>(hop_out) / ratio;
    const std::size_t tolerance = frame / 4;
    const std::size_t overlap = frame - hop_out;

    // Short material: a single frame cannot be searched; resample-free fallback
    // pads or cuts so the length is right.
    Channels out(channels, std::vector<float>(out_length + frame, 0.0f));
    std::vector<float> norm(out_length + frame, 0.0f);
    std::vector<float> window(frame);
    for (std::size_t i = 0; i < frame; ++i)
        window[i] = 0.5f - 0.5f * static_cast<float>(std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(i) /
                                                            static_cast<double>(frame)));

    std::vector<float> mono(length, 0.0f);
    for (const auto& c : in)
        for (std::size_t i = 0; i < length; ++i) mono[i] += c[i];

    auto sample = [&](const std::vector<float>& v, std::ptrdiff_t at) {
        return at >= 0 && static_cast<std::size_t>(at) < v.size() ? v[static_cast<std::size_t>(at)] : 0.0f;
    };

    std::ptrdiff_t prev = 0;
    for (std::size_t k = 0;; ++k) {
        const std::size_t out_at = k * hop_out;
        if (out_at >= out_length) break;
        const auto nominal = static_cast<std::ptrdiff_t>(std::llround(static_cast<double>(k) * hop_in));
        std::ptrdiff_t pos = nominal;
        if (k > 0) {
            // What would naturally follow the frame just written.
            const std::ptrdiff_t natural = prev + static_cast<std::ptrdiff_t>(hop_out);
            double best = -1e300;
            const auto lo = nominal - static_cast<std::ptrdiff_t>(tolerance);
            const auto hi = nominal + static_cast<std::ptrdiff_t>(tolerance);
            for (std::ptrdiff_t cand = lo; cand <= hi; cand += 2) {
                double corr = 0.0;
                for (std::size_t i = 0; i < overlap; i += 2)
                    corr += static_cast<double>(sample(mono, natural + static_cast<std::ptrdiff_t>(i))) *
                            sample(mono, cand + static_cast<std::ptrdiff_t>(i));
                if (corr > best) {
                    best = corr;
                    pos = cand;
                }
            }
        }
        for (std::size_t i = 0; i < frame; ++i) {
            const float w = window[i];
            norm[out_at + i] += w;
            for (std::size_t ch = 0; ch < channels; ++ch)
                out[ch][out_at + i] += w * sample(in[ch], pos + static_cast<std::ptrdiff_t>(i));
        }
        prev = pos;
    }
    for (std::size_t ch = 0; ch < channels; ++ch) {
        auto& o = out[ch];
        for (std::size_t i = 0; i < out_length; ++i) o[i] = norm[i] > 1e-3f ? o[i] / norm[i] : 0.0f;
        o.resize(out_length);
    }
    return out;
}

}  // namespace vcc::audio
