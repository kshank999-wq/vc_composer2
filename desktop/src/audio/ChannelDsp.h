// Console channel processing (Main Spec §12): the strip built into every
// track and bus, the built-in insert processors and hosted effect plug-ins.
//
// Threading: processors are created, prepared and destroyed on the message
// thread and processed only on the audio thread. Settings travel from the
// message thread to the audio thread through a lock-free Mailbox (a triple
// buffer), so a knob drag changes the sound at once, with no rebuild of the
// processing graph and no reset of filter, compressor, reverb or delay state.
// Nothing here allocates, locks or blocks while processing.
//
// Built-in insert kinds and their `InsertSlot::params` (missing names take
// the default shown):
//   "eq"          lf_freq 100, lf_gain 0, lf_q 0.7, lf_bell 0 (0 shelf / 1 bell),
//                 lmf_freq 400, lmf_gain 0, lmf_q 1, hmf_freq 2500, hmf_gain 0, hmf_q 1,
//                 hf_freq 8000, hf_gain 0, hf_q 0.7, hf_bell 0,
//                 hpf_hz 0 (0 = off), lpf_hz 0 (0 = off)            [gains in dB]
//   "compressor"  threshold_db -18, ratio 3, attack_ms 10, release_ms 120, makeup_db 0
//   "gate"        threshold_db -50, range_db 40, release_ms 150
//   "reverb"      room 0.82, damping 0.4, width 1, mix 1        [0..1; mix 1 = 100 % wet]
//   "delay"       time_ms 375 (1..2000), feedback 0.35 (0..0.95), mix 1
//   "limiter"     ceiling_db -0.3, release_ms 80
//   "saturation"  drive_db 6 (0..36), mix 1, output_db 0
//   "hardware"    outboard gear through the interface: send_db 0, return_db 0, mix 1,
//                 out_pair 2 (outputs 3-4), in_pair 2 (inputs 3-4), latency 0 (samples,
//                 the measured round trip; delay compensation aligns the rest to it)
// The reverb and delay defaults suit an effect return (100 % wet); lower
// `mix` when they are used as a channel insert.
// "plugin" hosts the effect named by `InsertSlot::plugin` (state restored
// from its saved chunk). Unknown kinds pass audio through unchanged.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <juce_audio_processors/juce_audio_processors.h>

#include "vcc/project/Project.h"

namespace vcc::desktop {

class PluginHost;

/// Single-writer (message thread) / single-reader (audio thread) triple
/// buffer for trivially copyable settings. Lock-free and allocation-free.
template <class T>
class Mailbox {
public:
    /// Writer side.
    void write(const T& value) noexcept
    {
        slots_[write_] = value;
        write_ = middle_.exchange(write_ | kDirty, std::memory_order_acq_rel) & kIndex;
    }
    /// Reader side: the newest value written (stable until the next call).
    const T& latest() noexcept
    {
        if (middle_.load(std::memory_order_acquire) & kDirty)
            read_ = middle_.exchange(read_, std::memory_order_acq_rel) & kIndex;
        return slots_[read_];
    }

private:
    static constexpr int kDirty = 4, kIndex = 3;
    T slots_[3]{};
    std::atomic<int> middle_{1};
    int write_ = 0;  // writer only
    int read_ = 2;   // reader only
};

/// Second-order IIR section (RBJ cookbook), transposed direct form II, stereo.
class Biquad {
public:
    void set_lowpass(double sr, double f, double q) noexcept;
    void set_highpass(double sr, double f, double q) noexcept;
    void set_peak(double sr, double f, double q, double gain_db) noexcept;
    void set_low_shelf(double sr, double f, double q, double gain_db) noexcept;
    void set_high_shelf(double sr, double f, double q, double gain_db) noexcept;
    void reset() noexcept { z1_[0] = z1_[1] = z2_[0] = z2_[1] = 0.0; }
    void process(float* data, int ch, int n) noexcept
    {
        double z1 = z1_[ch], z2 = z2_[ch];
        for (int i = 0; i < n; ++i) {
            const double x = data[i];
            const double y = b0_ * x + z1;
            z1 = b1_ * x - a1_ * y + z2;
            z2 = b2_ * x - a2_ * y;
            data[i] = static_cast<float>(y);
        }
        z1_[ch] = z1;
        z2_[ch] = z2;
    }

private:
    void set(double b0, double b1, double b2, double a0, double a1, double a2) noexcept;
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    double z1_[2]{}, z2_[2]{};
};

/// Feed-forward stereo-linked compressor (soft knee), reports gain reduction.
class Compressor {
public:
    void prepare(double sr) noexcept { sr_ = sr; }
    void reset() noexcept { env_db_ = 0.0f; }
    /// Returns the deepest gain reduction in this block (dB, <= 0). With a
    /// key (sidechain), the level is read from it instead of the signal.
    float process(float* l, float* r, int n, float threshold_db, float ratio, float attack_ms, float release_ms,
                  float makeup_db, const float* key_l = nullptr, const float* key_r = nullptr) noexcept;

private:
    double sr_ = 48000.0;
    float env_db_ = 0.0f;  // current gain change, dB (<= 0)
};

/// Downward expander/gate with a range, stereo-linked.
class Gate {
public:
    void prepare(double sr) noexcept { sr_ = sr; }
    void reset() noexcept { env_ = 0.0f; gain_ = 1.0f; hold_ = 0; }
    /// Returns true when the gate is closed (or closing) at the end of the block.
    /// With a key (sidechain), it opens on the key's level instead of the signal's.
    bool process(float* l, float* r, int n, float threshold_db, float range_db, float release_ms,
                 const float* key_l = nullptr, const float* key_r = nullptr) noexcept;

private:
    double sr_ = 48000.0;
    float env_ = 0.0f, gain_ = 1.0f;
    int hold_ = 0;
};

/// The console channel's built-in processing: trim, polarity, HPF/LPF
/// (12 dB/oct), four-band EQ and dynamics (compressor, gate), each section
/// with its own switch, dynamics before or after the EQ.
class ChannelProcessor {
public:
    void prepare(double sr);  ///< Message thread; keeps state when the rate is unchanged.
    /// Audio thread: stereo in place. A key (sidechain) drives the compressor
    /// and gate instead of the channel's own signal.
    void process(float* l, float* r, int n, const project::ChannelStrip& s, const float* key_l = nullptr,
                 const float* key_r = nullptr) noexcept;
    float gain_reduction_db() const noexcept { return gr_db_; }
    bool gate_closed() const noexcept { return gate_closed_; }

private:
    void update(const project::ChannelStrip& s) noexcept;
    void filters(float* l, float* r, int n, const project::ChannelStrip& s) noexcept;
    void eq(float* l, float* r, int n, const project::ChannelStrip& s) noexcept;
    void dynamics(float* l, float* r, int n, const project::ChannelStrip& s, const float* key_l, const float* key_r) noexcept;

    double sr_ = 0.0;
    project::ChannelStrip applied_{};
    bool valid_ = false;
    Biquad hpf_, lpf_, lf_, lmf_, hmf_, hf_;
    Compressor comp_;
    Gate gate_;
    float trim_ = 1.0f, trim_target_ = 1.0f;
    float gr_db_ = 0.0f;
    bool gate_closed_ = false;
};

class ParameterWatch;

/// An insert slot's processor.
class InsertProcessor {
public:
    static constexpr int kMaxAutomated = 16;

    virtual ~InsertProcessor() = default;
    /// Message thread. Must keep processing state when nothing changed.
    virtual void prepare(double sr, int max_block) = 0;
    /// Message thread: pushes the slot's settings (lock-free).
    virtual void set_params(const project::InsertSlot& slot) = 0;
    /// Audio thread: stereo in place.
    virtual void process(float* l, float* r, int n) noexcept = 0;
    /// Audio thread, while bypassed: built-ins pass audio through untouched;
    /// a plug-in is asked for its own bypass (which keeps its latency, so
    /// delay compensation stays aligned).
    virtual void process_bypassed(float*, float*, int) noexcept {}
    /// Why it is not processing ("Missing: ..."), or empty.
    virtual juce::String status() const { return {}; }
    virtual juce::AudioPluginInstance* plugin() noexcept { return nullptr; }
    /// Processing delay in samples (plug-ins report theirs; built-ins 0).
    virtual int latency_samples() const noexcept { return 0; }
    /// Message thread: listens to a hosted plug-in's parameter gestures
    /// (kept until the processor is destroyed, and released before the plug-in).
    virtual void set_watch(std::unique_ptr<ParameterWatch>) {}
    virtual bool has_watch() const noexcept { return false; }

    /// Audio thread: automation overrides one built-in parameter (index from
    /// insert_automation_index) until clear_automation().
    void automate(int index, float value) noexcept
    {
        if (index < 0 || index >= kMaxAutomated) return;
        automated_[static_cast<std::size_t>(index)] = value;
        automated_mask_ |= 1u << index;
    }
    void clear_automation() noexcept { automated_mask_ = 0; }
    /// Audio thread, before process(): this block's sidechain key (null:
    /// none). Only the built-in compressor and gate listen to it.
    void set_key(const float* l, const float* r) noexcept
    {
        key_l_ = l;
        key_r_ = r;
    }

    std::atomic<bool> bypass{false};

protected:
    const float* key_l_ = nullptr;
    const float* key_r_ = nullptr;
    float automated(int index, float fallback) const noexcept
    {
        return (automated_mask_ & (1u << index)) ? automated_[static_cast<std::size_t>(index)] : fallback;
    }
    std::array<float, kMaxAutomated> automated_{};
    std::uint32_t automated_mask_ = 0;
};

/// The audio interface as the hardware inserts see it during one chunk of
/// processing (audio thread only): the device's inputs and outputs, where in
/// the device block this chunk starts, and whether this is a live callback
/// at all (offline bounces have no interface: hardware inserts pass dry).
/// It also measures a round trip ("ping") for an outboard insert's latency.
struct HardwareIo {
    const float* const* inputs = nullptr;
    int num_inputs = 0;
    float* const* outputs = nullptr;
    int num_outputs = 0;
    int offset = 0;
    bool live = false;

    /// Message thread: sends a click out of `out_pair` and listens on
    /// `in_pair` for it to come back (pairs 1-based: 2 = channels 3-4).
    void request_ping(int out_pair, int in_pair) noexcept;
    /// The round trip in samples once heard; -1 while listening; -2 when
    /// nothing came back within a second (check the cables and levels).
    int ping_result() const noexcept { return ping_result_.load(); }
    bool pinging() const noexcept { return ping_state_.load() != 0; }
    /// Audio thread, after the chunk's mix: plays and listens for the ping.
    void service_ping(int n, double sample_rate) noexcept;

private:
    std::atomic<int> ping_state_{0};  // 0 idle, 1 requested, 2 listening
    std::atomic<int> ping_result_{0};
    std::atomic<int> ping_out_{2}, ping_in_{2};
    int ping_elapsed_ = 0;
};
HardwareIo& hardware_io() noexcept;

/// The automation index of a built-in insert's parameter (as named in
/// InsertSlot::params and the table above), or -1.
int insert_automation_index(const std::string& kind, const std::string& name);

/// Whether `kind` names a built-in processor (or "plugin").
bool is_known_insert_kind(const std::string& kind) noexcept;

/// Creates the processor for a slot (message thread). Plug-ins are created
/// through `host`; a plug-in that cannot be loaded gives a pass-through
/// processor whose status() says why. Returns nullptr for unknown kinds.
std::unique_ptr<InsertProcessor> create_insert(const project::InsertSlot& slot, PluginHost& host, double sr,
                                               int max_block);

/// Reads a named parameter with a default.
double insert_param(const project::InsertSlot& slot, const char* name, double fallback);

}  // namespace vcc::desktop
