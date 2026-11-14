#include "vcc/audio/Loudness.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace vcc::audio {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

double lufs_from_mean_square(double ms) noexcept
{
    return ms > 0.0 ? -0.691 + 10.0 * std::log10(ms) : LoudnessMeter::kSilence;
}

// --- K-weighting ---------------------------------------------------------------------------

void KWeighting::prepare(double fs) noexcept
{
    // Stage 1: the shelf. The analogue prototype BS.1770 tabulates at 48 kHz,
    // re-derived for `fs` by the bilinear transform.
    {
        const double f0 = 1681.974450955533, gain_db = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan(kPi * f0 / fs);
        const double vh = std::pow(10.0, gain_db / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        b1_ = {(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0};
        a1_ = {2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
    }
    // Stage 2: the RLB high pass.
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan(kPi * f0 / fs);
        const double a0 = 1.0 + k / q + k * k;
        a2_ = {2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
    }
    reset();
}

void KWeighting::reset() noexcept
{
    z1_ = {};
    z2_ = {};
}

// --- True peak -------------------------------------------------------------------------------

TruePeak::TruePeak()
{
    // A 48-tap low pass at the original Nyquist, at four times the rate
    // (Hann-windowed sinc), split into four phases each normalised to unity
    // gain, so a steady signal reads its own level.
    constexpr int taps = kFactor * kTapsPerPhase;
    const double centre = (taps - 1) / 2.0;
    std::array<double, taps> h{};
    for (int n = 0; n < taps; ++n) {
        const double t = (n - centre) / kFactor;
        const double sinc = std::abs(t) < 1e-12 ? 1.0 : std::sin(kPi * t) / (kPi * t);
        const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * (n + 0.5) / taps);
        h[static_cast<std::size_t>(n)] = sinc * window;
    }
    for (int p = 0; p < kFactor; ++p) {
        double sum = 0.0;
        for (int j = 0; j < kTapsPerPhase; ++j) sum += h[static_cast<std::size_t>(j * kFactor + p)];
        for (int j = 0; j < kTapsPerPhase; ++j)
            phases_[static_cast<std::size_t>(p)][static_cast<std::size_t>(j)] =
                static_cast<float>(h[static_cast<std::size_t>(j * kFactor + p)] / sum);
    }
    reset();
}

void TruePeak::reset() noexcept
{
    history_ = {};
    head_ = 0;
}

float TruePeak::process(float x) noexcept
{
    history_[static_cast<std::size_t>(head_)] = x;
    float peak = 0.0f;
    for (int p = 0; p < kFactor; ++p) {
        const auto& c = phases_[static_cast<std::size_t>(p)];
        float y = 0.0f;
        int idx = head_;
        for (int j = 0; j < kTapsPerPhase; ++j) {
            y += c[static_cast<std::size_t>(j)] * history_[static_cast<std::size_t>(idx)];
            idx = idx == 0 ? kTapsPerPhase - 1 : idx - 1;
        }
        peak = std::max(peak, std::abs(y));
    }
    head_ = (head_ + 1) % kTapsPerPhase;
    return peak;
}

// --- Analyser --------------------------------------------------------------------------------

void LoudnessAnalyser::prepare(double sample_rate, int channels)
{
    channels_ = std::clamp(channels, 1, kMaxChannels);
    block_ = std::max(1, static_cast<int>(std::lround(sample_rate * 0.1)));
    for (auto& w : weighting_) w.prepare(sample_rate);
    reset();
}

void LoudnessAnalyser::reset() noexcept
{
    for (auto& w : weighting_) w.reset();
    for (auto& p : peaks_) p.reset();
    count_ = 0;
    sum_ = 0.0;
    true_peak_ = sample_peak_ = 0.0f;
}

// --- Meter -----------------------------------------------------------------------------------

void LoudnessMeter::reset()
{
    recent_.clear();
    windows_.clear();
    blocks_ = 0;
    true_peak_ = sample_peak_ = 0.0f;
    max_momentary_ = kSilence;
}

void LoudnessMeter::add(const LoudnessBlock& b)
{
    recent_.push_back(b.mean_square);
    if (recent_.size() > 30) recent_.pop_front();
    ++blocks_;
    true_peak_ = std::max(true_peak_, b.true_peak);
    sample_peak_ = std::max(sample_peak_, b.sample_peak);
    // Each 100 ms completes a 400 ms gating window (75% overlap).
    if (recent_.size() >= 4) {
        const double ms = mean_of_last(4);
        windows_.push_back(ms);
        max_momentary_ = std::max(max_momentary_, lufs_from_mean_square(ms));
    }
}

double LoudnessMeter::mean_of_last(std::size_t n) const
{
    n = std::min(n, recent_.size());
    if (n == 0) return 0.0;
    return std::accumulate(recent_.end() - static_cast<std::ptrdiff_t>(n), recent_.end(), 0.0) / static_cast<double>(n);
}

double LoudnessMeter::momentary() const { return recent_.size() < 4 ? kSilence : lufs_from_mean_square(mean_of_last(4)); }

double LoudnessMeter::short_term() const { return recent_.size() < 30 ? kSilence : lufs_from_mean_square(mean_of_last(30)); }

double LoudnessMeter::integrated() const
{
    // Absolute gate at -70 LUFS, then a relative gate 10 LU under what passed.
    const double absolute = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    double sum = 0.0;
    std::size_t n = 0;
    for (double w : windows_)
        if (w > absolute) {
            sum += w;
            ++n;
        }
    if (n == 0) return kSilence;
    const double relative = sum / static_cast<double>(n) * std::pow(10.0, -10.0 / 10.0);
    sum = 0.0;
    n = 0;
    for (double w : windows_)
        if (w > absolute && w > relative) {
            sum += w;
            ++n;
        }
    return n == 0 ? kSilence : lufs_from_mean_square(sum / static_cast<double>(n));
}

double LoudnessMeter::true_peak_db() const { return true_peak_ > 0.0f ? 20.0 * std::log10(true_peak_) : kSilence; }

double LoudnessMeter::sample_peak_db() const { return sample_peak_ > 0.0f ? 20.0 * std::log10(sample_peak_) : kSilence; }

}  // namespace vcc::audio
