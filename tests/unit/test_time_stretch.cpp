// Clip processing: WSOLA time stretch keeps pitch and level; reverse.
#include <cmath>

#include "vcc/audio/TimeStretch.h"
#include "vcc_test.h"

using namespace vcc;

namespace {
constexpr double kSr = 48000.0;
std::vector<float> sine(double hz, double seconds, float amp = 0.5f)
{
    std::vector<float> v(static_cast<std::size_t>(seconds * kSr));
    for (std::size_t i = 0; i < v.size(); ++i)
        v[i] = amp * static_cast<float>(std::sin(2.0 * 3.141592653589793 * hz * static_cast<double>(i) / kSr));
    return v;
}
/// Frequency from rising zero crossings over the middle of the signal.
double frequency(const std::vector<float>& v)
{
    const std::size_t a = v.size() / 4, b = 3 * v.size() / 4;
    int crossings = 0;
    for (std::size_t i = a + 1; i < b; ++i)
        if (v[i - 1] < 0.0f && v[i] >= 0.0f) ++crossings;
    return crossings / (static_cast<double>(b - a) / kSr);
}
double rms(const std::vector<float>& v, std::size_t a, std::size_t b)
{
    double s = 0.0;
    for (std::size_t i = a; i < b; ++i) s += static_cast<double>(v[i]) * v[i];
    return std::sqrt(s / static_cast<double>(b - a));
}
}  // namespace

TEST_CASE("time stretch: length follows the ratio, pitch and level do not")
{
    for (double ratio : {0.5, 0.8, 1.25, 1.5, 2.0}) {
        const audio::Channels in{sine(440.0, 2.0), sine(440.0, 2.0, 0.25f)};
        const auto out = audio::time_stretch(in, ratio, kSr);
        REQUIRE(out.size() == 2u);
        CHECK_EQ(out[0].size(), static_cast<std::size_t>(std::llround(static_cast<double>(in[0].size()) * ratio)));
        CHECK_NEAR(frequency(out[0]), 440.0, 3.0);
        // Level is kept, without the dips of a naive overlap-add.
        const double level = rms(out[0], out[0].size() / 4, 3 * out[0].size() / 4);
        CHECK_NEAR(level, 0.5 / std::sqrt(2.0), 0.03);
        double worst = 1.0;
        const std::size_t w = 2400;
        for (std::size_t a = out[0].size() / 4; a + w < 3 * out[0].size() / 4; a += w) worst = std::min(worst, rms(out[0], a, a + w));
        CHECK(worst > 0.30);
        // The channels keep their balance.
        CHECK_NEAR(rms(out[1], out[1].size() / 4, 3 * out[1].size() / 4) / level, 0.5, 0.05);
    }
    // Unchanged at 1.0; clamped; empty stays empty.
    const audio::Channels one{sine(100.0, 0.5)};
    CHECK(audio::time_stretch(one, 1.0, kSr) == one);
    CHECK_EQ(audio::time_stretch(one, 10.0, kSr)[0].size(), one[0].size() * 4);
    CHECK(audio::time_stretch({}, 2.0, kSr).empty());
}

TEST_CASE("reverse plays the audio backwards")
{
    const audio::Channels in{{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}};
    const auto out = audio::reversed(in);
    CHECK(out[0] == (std::vector<float>{3.0f, 2.0f, 1.0f}));
    CHECK(out[1] == (std::vector<float>{6.0f, 5.0f, 4.0f}));
}
