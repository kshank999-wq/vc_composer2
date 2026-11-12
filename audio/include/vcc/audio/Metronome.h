// Copyright (c) VC Composer. All rights reserved.
//
// A click to write to.
//
// **What this is not.** It is not a second metronome running alongside Pro
// Tools'. Two clicks from two applications share no clock: they would drift
// apart and phase against each other, and a click that is not in time with
// what the composer is hearing is worse than no click at all. So this one
// refuses to run while the host transport is playing (`app/MetronomeCommand.h`),
// and what it is for is the rest of the time - writing at a desk, with or
// without a session open, wanting a tempo to feel.
//
// **Real-time rules.** `render` runs on an audio callback: it allocates
// nothing, locks nothing, and touches no `std::string`. Settings arrive
// through plain atomics and take effect at the start of the next block,
// which for a metronome is soon enough.
//
// **No drift.** Click positions are computed as `beat * samples_per_beat`
// from an absolute sample count rather than by adding an interval each
// time, so the hundredth beat is where the hundredth beat should be and not
// a hundred roundings away from it.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "vcc/music/MusicalTime.h"

namespace vcc::audio {

/// Tempi outside this are refused rather than clamped: 1000 beats a minute
/// is not a metronome and 4 is not a tempo anyone is counting.
constexpr double kSlowestMetronomeTempo = 20.0;
constexpr double kFastestMetronomeTempo = 300.0;

/// How loud a click is, as a fraction of full scale. Quiet enough not to
/// startle, loud enough to hear over a keyboard.
constexpr float kDefaultMetronomeLevel = 0.25f;

/// A click, and whether it is the one that starts the bar.
struct MetronomeClick {
    /// Sample offset from the start of the block it falls in.
    std::size_t frame = 0;

    /// Which beat of the bar, counted from zero.
    std::uint32_t beat_in_bar = 0;

    bool is_downbeat() const noexcept { return beat_in_bar == 0; }
};

class Metronome {
public:
    Metronome();

    // --- Setting up, from the message thread ---------------------------------

    /// Tells it the device's sample rate. Silences and rewinds it: a click
    /// counted in samples means nothing across a change of rate.
    void prepare(double sample_rate) noexcept;

    double sample_rate() const noexcept { return sample_rate_.load(std::memory_order_relaxed); }

    /// The tempo in beats a minute. Refused, and left as it was, outside
    /// `kSlowestMetronomeTempo` to `kFastestMetronomeTempo`.
    bool set_tempo(double beats_per_minute) noexcept;
    double tempo() const noexcept { return tempo_.load(std::memory_order_relaxed); }

    /// How many beats to a bar, which is what decides where the accent
    /// falls. A meter the music does not have is refused.
    bool set_meter(music::TimeSignature meter) noexcept;
    music::TimeSignature meter() const noexcept;

    void set_level(float level) noexcept;
    float level() const noexcept { return level_.load(std::memory_order_relaxed); }

    /// Starts from the top of a bar, so the first click a composer hears is
    /// a downbeat. Starting mid-bar would make them count from the wrong
    /// place.
    void start() noexcept;
    void stop() noexcept;
    bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }

    // --- On the audio thread -------------------------------------------------

    /// Fills `frames` of interleaved stereo, **replacing** what is there.
    ///
    /// Silence when it is not running, which is a buffer of zeroes rather
    /// than whatever was in it - a caller mixing this in has its own add.
    void render(float* interleaved_stereo, std::size_t frames) noexcept;

    /// Where the clicks in the next `frames` would fall, without rendering
    /// them and without advancing. For a test, and for anything that wants
    /// to schedule rather than play.
    ///
    /// Returns how many it wrote, which is never more than `capacity`.
    std::size_t clicks_in(std::size_t frames, MetronomeClick* out,
                          std::size_t capacity) const noexcept;

    /// Absolute samples rendered since it started.
    std::uint64_t position() const noexcept
    {
        return position_.load(std::memory_order_relaxed);
    }

    /// Samples between beats at the current tempo and rate. Zero when it
    /// has not been prepared.
    double samples_per_beat() const noexcept;

private:
    /// Where beat `beat` falls, in absolute samples.
    std::uint64_t sample_of_beat(std::uint64_t beat) const noexcept;

    std::atomic<double> sample_rate_{0.0};
    std::atomic<double> tempo_{120.0};
    std::atomic<std::uint16_t> beats_per_bar_{4};
    std::atomic<std::uint16_t> beat_unit_{4};
    std::atomic<float> level_{kDefaultMetronomeLevel};
    std::atomic<bool> running_{false};

    std::atomic<std::uint64_t> position_{0};
};

}  // namespace vcc::audio
