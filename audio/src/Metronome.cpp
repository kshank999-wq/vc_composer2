#include "vcc/audio/Metronome.h"

#include <cstddef>
#include <cmath>

namespace vcc::audio {
namespace {

/// How long a click lasts. Short enough to be a tick rather than a note,
/// long enough to survive a small speaker.
constexpr double kClickSeconds = 0.03;

/// The two pitches. A fifth apart, which is enough to tell the start of a
/// bar from the rest of it without either sounding like music.
constexpr double kDownbeatHz = 1600.0;
constexpr double kBeatHz = 1066.0;

constexpr double kTwoPi = 6.283185307179586;

}  // namespace

Metronome::Metronome() = default;

void Metronome::prepare(double sample_rate) noexcept
{
    // Silenced rather than re-counted: a position in samples means nothing
    // once the samples are a different length.
    running_.store(false, std::memory_order_relaxed);
    position_.store(0, std::memory_order_relaxed);
    sample_rate_.store(sample_rate > 0.0 ? sample_rate : 0.0, std::memory_order_relaxed);
}

bool Metronome::set_tempo(double beats_per_minute) noexcept
{
    if (!(beats_per_minute >= kSlowestMetronomeTempo)
        || !(beats_per_minute <= kFastestMetronomeTempo))
        return false;

    tempo_.store(beats_per_minute, std::memory_order_relaxed);
    return true;
}

bool Metronome::set_meter(music::TimeSignature meter) noexcept
{
    if (!meter.is_valid())
        return false;

    beats_per_bar_.store(meter.numerator, std::memory_order_relaxed);
    beat_unit_.store(meter.denominator, std::memory_order_relaxed);
    return true;
}

music::TimeSignature Metronome::meter() const noexcept
{
    music::TimeSignature meter;
    meter.numerator = beats_per_bar_.load(std::memory_order_relaxed);
    meter.denominator = beat_unit_.load(std::memory_order_relaxed);
    return meter;
}

void Metronome::set_level(float level) noexcept
{
    const float clamped = level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
    level_.store(clamped, std::memory_order_relaxed);
}

void Metronome::start() noexcept
{
    // From the top of a bar, so the first click is a downbeat and a
    // composer counts from where the click says they are.
    position_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
}

void Metronome::stop() noexcept
{
    running_.store(false, std::memory_order_relaxed);
}

double Metronome::samples_per_beat() const noexcept
{
    const double rate = sample_rate_.load(std::memory_order_relaxed);
    const double tempo = tempo_.load(std::memory_order_relaxed);
    if (rate <= 0.0 || tempo <= 0.0)
        return 0.0;

    // The beat the metronome counts is the one the meter names: a quarter
    // in 4/4, an eighth in 6/8. A click counting quarters in 6/8 would be
    // counting something the music is not written in.
    const double unit = static_cast<double>(beat_unit_.load(std::memory_order_relaxed));
    const double quarters_per_beat = unit > 0.0 ? 4.0 / unit : 1.0;
    return rate * 60.0 / tempo * quarters_per_beat;
}

std::uint64_t Metronome::sample_of_beat(std::uint64_t beat) const noexcept
{
    // Multiplied out from beat zero rather than added up beat by beat: an
    // interval added a thousand times is a thousand roundings, and a
    // metronome that drifts is not one.
    //
    // The `+ 0.5` rounds instead of truncating, which halves the worst-case
    // error from one sample to half of one. Deliberately below what any
    // test asserts: the drift test allows a sample either way, because
    // demanding exactness from a floating-point product would be testing
    // the arithmetic rather than the metronome. A mutation dropping this
    // survives the suite for that reason, and should - twenty microseconds
    // at 48 kHz is not something a composer hears.
    const double spb = samples_per_beat();
    if (spb <= 0.0)
        return 0;
    return static_cast<std::uint64_t>(static_cast<double>(beat) * spb + 0.5);
}

std::size_t Metronome::clicks_in(std::size_t frames, MetronomeClick* out,
                                 std::size_t capacity) const noexcept
{
    if (out == nullptr || capacity == 0 || frames == 0)
        return 0;
    if (!running_.load(std::memory_order_relaxed))
        return 0;

    const double spb = samples_per_beat();
    if (spb <= 0.0)
        return 0;

    const std::uint64_t start = position_.load(std::memory_order_relaxed);
    const std::uint64_t end = start + frames;

    // The first beat at or after the start of the block. Floor, then step
    // forward while it is behind - cheaper than a search and exact.
    auto beat = static_cast<std::uint64_t>(static_cast<double>(start) / spb);
    while (beat > 0 && sample_of_beat(beat) > start)
        --beat;
    while (sample_of_beat(beat) < start)
        ++beat;

    const std::uint32_t per_bar = beats_per_bar_.load(std::memory_order_relaxed);

    std::size_t written = 0;
    for (; written < capacity; ++beat) {
        const std::uint64_t at = sample_of_beat(beat);
        if (at >= end)
            break;

        MetronomeClick click;
        click.frame = static_cast<std::size_t>(at - start);
        click.beat_in_bar = per_bar > 0 ? static_cast<std::uint32_t>(beat % per_bar) : 0;
        out[written] = click;
        ++written;
    }

    return written;
}

void Metronome::render(float* interleaved_stereo, std::size_t frames) noexcept
{
    if (interleaved_stereo == nullptr || frames == 0)
        return;

    for (std::size_t i = 0; i < frames * 2; ++i)
        interleaved_stereo[i] = 0.0f;

    if (!running_.load(std::memory_order_relaxed))
        return;

    const double rate = sample_rate_.load(std::memory_order_relaxed);
    const double spb = samples_per_beat();
    if (rate <= 0.0 || spb <= 0.0)
        return;

    const std::uint64_t start = position_.load(std::memory_order_relaxed);
    const auto click_frames = static_cast<std::uint64_t>(kClickSeconds * rate);
    const float gain = level_.load(std::memory_order_relaxed);
    const std::uint32_t per_bar = beats_per_bar_.load(std::memory_order_relaxed);

    // Every beat whose click could still be sounding in this block,
    // including one that began before it: a click is longer than nothing,
    // and a block boundary is not a reason to cut it off.
    auto beat = static_cast<std::uint64_t>(static_cast<double>(start) / spb);
    while (beat > 0 && sample_of_beat(beat) + click_frames > start)
        --beat;

    for (;; ++beat) {
        const std::uint64_t at = sample_of_beat(beat);
        if (at >= start + frames)
            break;
        if (at + click_frames <= start)
            continue;

        const bool downbeat = per_bar > 0 && beat % per_bar == 0;
        const double hz = downbeat ? kDownbeatHz : kBeatHz;
        const double step = kTwoPi * hz / rate;

        for (std::uint64_t n = 0; n < click_frames; ++n) {
            const std::uint64_t sample = at + n;
            if (sample < start)
                continue;
            if (sample >= start + frames)
                break;

            // A sine under a straight decay: a click with no envelope is a
            // pop, and a pop is what a composer turns off.
            const double fade =
                1.0 - static_cast<double>(n) / static_cast<double>(click_frames);
            const auto value =
                static_cast<float>(std::sin(step * static_cast<double>(n)) * fade) * gain;

            const std::size_t frame = static_cast<std::size_t>(sample - start);
            interleaved_stereo[frame * 2] = value;
            interleaved_stereo[frame * 2 + 1] = value;
        }
    }

    position_.store(start + frames, std::memory_order_relaxed);
}

}  // namespace vcc::audio
