// Loudness and true peak (ITU-R BS.1770-4, EBU R 128 / Tech 3341), for the
// master meters and the master export.
//
// Split in two so the audio thread does only fixed work: the analyser runs
// per sample (K-weighting, true-peak oversampling) and hands over one small
// record per 100 ms; the meter, on any other thread, turns those records
// into momentary (400 ms), short-term (3 s) and gated integrated loudness.
#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <vector>

namespace vcc::audio {

/// BS.1770's K-weighting for one channel: a high shelf (head effects) then a
/// high pass (the RLB curve), with coefficients derived for any sample rate.
class KWeighting {
public:
    void prepare(double sample_rate) noexcept;
    void reset() noexcept;
    double process(double x) noexcept
    {
        // Direct form II transposed, two stages.
        const double y1 = b1_[0] * x + z1_[0];
        z1_[0] = b1_[1] * x - a1_[0] * y1 + z1_[1];
        z1_[1] = b1_[2] * x - a1_[1] * y1;
        const double y2 = y1 + z2_[0];
        z2_[0] = -2.0 * y1 - a2_[0] * y2 + z2_[1];
        z2_[1] = y1 - a2_[1] * y2;
        return y2;
    }

private:
    std::array<double, 3> b1_{};
    std::array<double, 2> a1_{}, a2_{};
    std::array<double, 2> z1_{}, z2_{};
};

/// The true (inter-sample) peak of one channel, by 4x oversampling through a
/// 48-tap windowed-sinc interpolator, as BS.1770 Annex 2 describes.
class TruePeak {
public:
    static constexpr int kFactor = 4;
    static constexpr int kTapsPerPhase = 12;
    TruePeak();
    void reset() noexcept;
    /// The largest absolute value of the sample and the three points interpolated before it.
    float process(float x) noexcept;

private:
    std::array<std::array<float, kTapsPerPhase>, kFactor> phases_{};
    std::array<float, kTapsPerPhase> history_{};
    int head_ = 0;
};

/// What 100 ms of audio contributed: the channel-summed mean square of the
/// K-weighted signal (BS.1770's G_i weights: 1 for L, R and C), and the
/// largest true and sample peaks.
struct LoudnessBlock {
    double mean_square = 0.0;
    float true_peak = 0.0f;
    float sample_peak = 0.0f;
};

/// The per-sample part (real-time safe once prepared).
class LoudnessAnalyser {
public:
    static constexpr int kMaxChannels = 2;
    void prepare(double sample_rate, int channels);
    void reset() noexcept;
    /// Feeds `n` samples of each channel; calls `emit` for every 100 ms completed.
    template <typename F> void process(const float* const* channels, int n, F&& emit) noexcept
    {
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < channels_; ++c) {
                const float x = channels[c][i];
                const double k = weighting_[static_cast<std::size_t>(c)].process(static_cast<double>(x));
                sum_ += k * k;
                const float tp = peaks_[static_cast<std::size_t>(c)].process(x);
                if (tp > true_peak_) true_peak_ = tp;
                const float ax = x < 0.0f ? -x : x;
                if (ax > sample_peak_) sample_peak_ = ax;
            }
            if (++count_ >= block_) {
                emit(LoudnessBlock{sum_ / static_cast<double>(block_), true_peak_, sample_peak_});
                sum_ = 0.0;
                count_ = 0;
                true_peak_ = 0.0f;
                sample_peak_ = 0.0f;
            }
        }
    }

private:
    std::array<KWeighting, kMaxChannels> weighting_{};
    std::array<TruePeak, kMaxChannels> peaks_{};
    int channels_ = 2;
    int block_ = 4800;
    int count_ = 0;
    double sum_ = 0.0;
    float true_peak_ = 0.0f, sample_peak_ = 0.0f;
};

/// The readings (not real-time: it keeps every 400 ms window for gating).
class LoudnessMeter {
public:
    static constexpr double kSilence = -200.0;  ///< Reported for "no reading" (-inf LUFS).
    void add(const LoudnessBlock& b);
    void reset();
    /// LUFS over the last 400 ms and 3 s.
    double momentary() const;
    double short_term() const;
    /// Gated integrated loudness since the reset (absolute gate -70 LUFS, relative -10 LU).
    double integrated() const;
    /// The highest true and sample peaks since the reset, dBTP / dBFS.
    double true_peak_db() const;
    double sample_peak_db() const;
    double max_momentary() const noexcept { return max_momentary_; }
    double seconds() const noexcept { return static_cast<double>(blocks_) * 0.1; }

private:
    double mean_of_last(std::size_t n) const;
    std::deque<double> recent_;      ///< Last 30 blocks' mean squares.
    std::vector<double> windows_;    ///< Every 400 ms window's mean square (75% overlap).
    std::size_t blocks_ = 0;
    float true_peak_ = 0.0f, sample_peak_ = 0.0f;
    double max_momentary_ = kSilence;
};

/// Mean square to LUFS (BS.1770: -0.691 + 10 log10).
double lufs_from_mean_square(double ms) noexcept;

}  // namespace vcc::audio
