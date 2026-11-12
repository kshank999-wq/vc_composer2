// Real-time pieces of automation playback and plug-in delay compensation
// (Main Spec §12), shared by the playback engine and the hosted-plug-in
// wrappers. Everything here that runs on the audio thread is allocation- and
// lock-free; buffers are sized on the message thread.
//
//   * Curve: an automation lane rendered to sample positions on the message
//     thread (through the tempo map), evaluated on the audio thread.
//   * DelayLine: a stereo ring buffer delaying one routing edge (track ->
//     bus, send, bus -> bus) so every path reaches the master aligned.
//   * GestureQueue / ParameterWatch: a hosted plug-in's own editor reports
//     parameter gestures and values on whatever thread it likes; they are
//     queued lock-free and handled on the message thread.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "vcc/project/Project.h"

namespace vcc::desktop {

// --- Curves ------------------------------------------------------------------------------

struct CurvePoint {
    std::int64_t sample = 0;
    double value = 0.0;
    std::uint8_t shape = 0;  ///< 0 linear, 1 step, 2 smooth (to the next point)
};
using Curve = std::vector<CurvePoint>;

/// Renders a lane to samples. Linear and smooth segments that a tempo change
/// bends are subdivided, so the curve follows the lane in ticks.
Curve make_curve(const project::AutomationLane& lane, const TempoMap& tempo, double sample_rate, bool toggle);
/// A 0/1 mute lane as a gain curve (1 = open): every mute closes over `fade`
/// samples ending exactly on its point, every unmute opens over `fade`
/// samples starting on its point.
Curve make_mute_gain_curve(const project::AutomationLane& lane, const TempoMap& tempo, double sample_rate,
                           std::int64_t fade);
/// The curve's value at `sample` (audio thread; binary search, no allocation).
double curve_value(const Curve& c, std::int64_t sample) noexcept;
/// Multiplies l/r by the curve over [from, from + n), sample-accurately.
void apply_curve_gain(const Curve& c, std::int64_t from, float* l, float* r, int n) noexcept;

// --- Channel strip fields ------------------------------------------------------------------

/// Index of a "strip.<field>" param for set_strip_field(), or -1.
int strip_field_index(const std::string& param);
/// Audio thread: sets one numeric field or switch of a ChannelStrip.
void set_strip_field(project::ChannelStrip& s, int index, double value) noexcept;

// --- Delay lines ---------------------------------------------------------------------------

/// Stereo delay for one routing edge. Capacity is fixed at construction
/// (message thread); the delay may be anything up to capacity - block.
class DelayLine {
public:
    explicit DelayLine(int capacity);
    int capacity() const noexcept { return mask_ + 1; }
    /// In place: l/r become what went in `delay` samples ago.
    void process(float* l, float* r, int n, int delay) noexcept;
    /// Writes l/r times a gain ramp (from -> to over the block), then adds
    /// the delayed output to dl/dr.
    void send(const float* l, const float* r, int n, float from_l, float to_l, float from_r, float to_r, float* dl,
              float* dr, int delay) noexcept;
    /// Writes silence (the edge is bypassed this block).
    void silence(int n) noexcept;
    void clear() noexcept;

private:
    juce::AudioBuffer<float> ring_;
    int mask_ = 0;
    int write_ = 0;
};

// --- Plug-in parameter gestures --------------------------------------------------------------

struct GestureEvent {
    int watch = 0;     ///< ParameterWatch id
    int param = 0;     ///< Parameter index in the plug-in's getParameters()
    int kind = 0;      ///< 0 value, 1 gesture start, 2 gesture end
    float value = 0.0f;
    std::int64_t at = 0;  ///< Heard position (samples) when it happened
};

/// Bounded multi-producer / single-consumer queue (Vyukov). Producers are
/// any thread; the consumer is the message thread. Lock- and allocation-free.
class GestureQueue {
public:
    static constexpr std::size_t kSize = 1024;
    GestureQueue();
    bool push(const GestureEvent& e) noexcept;
    bool pop(GestureEvent& e) noexcept;
    /// The heard position producers stamp events with (set by the engine).
    std::atomic<std::int64_t> heard{0};

private:
    struct Cell {
        std::atomic<std::size_t> seq{0};
        GestureEvent data;
    };
    std::array<Cell, kSize> cells_;
    std::atomic<std::size_t> head_{0};
    std::size_t tail_ = 0;
};

/// Listens to every parameter of a hosted plug-in. Owned by the plug-in's
/// wrapper and destroyed before the instance.
class ParameterWatch final : private juce::AudioProcessorParameter::Listener {
public:
    ParameterWatch(juce::AudioProcessor& processor, std::shared_ptr<GestureQueue> queue, int id);
    ~ParameterWatch() override;
    int id() const noexcept { return id_; }

private:
    void parameterValueChanged(int index, float value) override;
    void parameterGestureChanged(int index, bool starting) override;

    juce::AudioProcessor& processor_;
    std::shared_ptr<GestureQueue> queue_;
    int id_ = 0;
};

}  // namespace vcc::desktop
