// Loudness and true peak against EBU Tech 3341's reference cases (BS.1770-4).
#include <cmath>
#include <cstdio>
#include <vector>

#include "vcc/audio/Loudness.h"
#include "vcc_test.h"

using namespace vcc::audio;

namespace {
constexpr double kPi = 3.14159265358979323846;

/// Stereo sections of a sine: {seconds, dBFS peak}.
LoudnessMeter measure(double fs, double freq, const std::vector<std::pair<double, double>>& sections, double phase = 0.0)
{
    LoudnessAnalyser a;
    a.prepare(fs, 2);
    LoudnessMeter m;
    std::vector<float> l(1024), r(1024);
    long long t = 0;
    for (const auto& [seconds, db] : sections) {
        const double amp = db <= -150.0 ? 0.0 : std::pow(10.0, db / 20.0);
        long long left = static_cast<long long>(seconds * fs);
        while (left > 0) {
            const int n = static_cast<int>(std::min<long long>(1024, left));
            for (int i = 0; i < n; ++i, ++t) {
                const float v = static_cast<float>(amp * std::sin(2.0 * kPi * freq * static_cast<double>(t) / fs + phase));
                l[static_cast<std::size_t>(i)] = v;
                r[static_cast<std::size_t>(i)] = v;
            }
            const float* ch[] = {l.data(), r.data()};
            a.process(ch, n, [&](const LoudnessBlock& b) { m.add(b); });
            left -= n;
        }
    }
    return m;
}
}  // namespace

TEST_CASE("a stereo 1 kHz sine at -23 dBFS reads -23 LUFS (Tech 3341 case 1)")
{
    const auto m = measure(48000.0, 1000.0, {{20.0, -23.0}});
    CHECK(std::abs(m.integrated() + 23.0) < 0.1);
    CHECK(std::abs(m.momentary() + 23.0) < 0.1);
    CHECK(std::abs(m.short_term() + 23.0) < 0.1);
}

TEST_CASE("-33 dBFS reads -33 LUFS (case 2), at 44.1 kHz too")
{
    CHECK(std::abs(measure(48000.0, 1000.0, {{20.0, -33.0}}).integrated() + 33.0) < 0.1);
    CHECK(std::abs(measure(44100.0, 1000.0, {{20.0, -23.0}}).integrated() + 23.0) < 0.1);
    CHECK(std::abs(measure(96000.0, 1000.0, {{20.0, -23.0}}).integrated() + 23.0) < 0.1);
}

TEST_CASE("the relative gate leaves quiet passages out (case 3)")
{
    // -36 / -23 / -36: the -36 sections fall under the -33 relative gate.
    const auto m = measure(48000.0, 1000.0, {{10.0, -36.0}, {60.0, -23.0}, {10.0, -36.0}});
    CHECK(std::abs(m.integrated() + 23.0) < 0.1);
}

TEST_CASE("the absolute gate leaves silence out (case 4/5)")
{
    const auto m = measure(48000.0, 1000.0, {{20.0, -72.0}, {20.0, -36.0}, {20.0, -72.0}, {5.0, -200.0}});
    CHECK(std::abs(m.integrated() + 36.0) < 0.1);
    CHECK(measure(48000.0, 1000.0, {{5.0, -200.0}}).integrated() <= LoudnessMeter::kSilence);
}

TEST_CASE("true peak finds the peak between the samples")
{
    // A quarter-rate sine sampled 45 degrees off its peaks: every sample is
    // at 0.707 (-3 dBFS) while the wave itself reaches 0 dBTP.
    const auto m = measure(48000.0, 12000.0, {{1.0, 0.0}}, kPi / 4.0);
    CHECK(std::abs(m.sample_peak_db() + 3.01) < 0.05);
    CHECK(std::abs(m.true_peak_db()) < 0.3);
    // A low tone: true and sample peaks agree.
    const auto low = measure(48000.0, 100.0, {{1.0, -6.0}});
    CHECK(std::abs(low.true_peak_db() + 6.0) < 0.1);
}
