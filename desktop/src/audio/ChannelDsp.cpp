#include "audio/ChannelDsp.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio/AutomationRt.h"
#include "audio/PluginHost.h"

namespace vcc::desktop {

namespace {

constexpr double kPi = 3.14159265358979323846;

inline float db_to_lin(float db) noexcept { return std::exp(db * 0.11512925464970229f); }  // ln(10)/20

double clamp_freq(double sr, double f) noexcept { return std::clamp(f, 10.0, sr * 0.49); }

void ramp_gain(float* data, int n, float from, float to) noexcept
{
    if (juce::exactlyEqual(from, to)) {
        if (!juce::exactlyEqual(from, 1.0f)) juce::FloatVectorOperations::multiply(data, from, n);
        return;
    }
    const float step = (to - from) / static_cast<float>(std::max(1, n));
    float g = from;
    for (int i = 0; i < n; ++i) {
        g += step;
        data[i] *= g;
    }
}

}  // namespace

// --- Biquad ----------------------------------------------------------------------------

void Biquad::set(double b0, double b1, double b2, double a0, double a1, double a2) noexcept
{
    b0_ = b0 / a0;
    b1_ = b1 / a0;
    b2_ = b2 / a0;
    a1_ = a1 / a0;
    a2_ = a2 / a0;
}

void Biquad::set_lowpass(double sr, double f, double q) noexcept
{
    const double w = 2.0 * kPi * clamp_freq(sr, f) / sr, c = std::cos(w), alpha = std::sin(w) / (2.0 * std::max(0.1, q));
    set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

void Biquad::set_highpass(double sr, double f, double q) noexcept
{
    const double w = 2.0 * kPi * clamp_freq(sr, f) / sr, c = std::cos(w), alpha = std::sin(w) / (2.0 * std::max(0.1, q));
    set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

void Biquad::set_peak(double sr, double f, double q, double gain_db) noexcept
{
    const double A = std::pow(10.0, gain_db / 40.0);
    const double w = 2.0 * kPi * clamp_freq(sr, f) / sr, c = std::cos(w), alpha = std::sin(w) / (2.0 * std::max(0.1, q));
    set(1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A);
}

void Biquad::set_low_shelf(double sr, double f, double q, double gain_db) noexcept
{
    const double A = std::pow(10.0, gain_db / 40.0);
    const double w = 2.0 * kPi * clamp_freq(sr, f) / sr, c = std::cos(w), alpha = std::sin(w) / (2.0 * std::max(0.1, q));
    const double s = 2.0 * std::sqrt(A) * alpha;
    set(A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
        (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
}

void Biquad::set_high_shelf(double sr, double f, double q, double gain_db) noexcept
{
    const double A = std::pow(10.0, gain_db / 40.0);
    const double w = 2.0 * kPi * clamp_freq(sr, f) / sr, c = std::cos(w), alpha = std::sin(w) / (2.0 * std::max(0.1, q));
    const double s = 2.0 * std::sqrt(A) * alpha;
    set(A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
        (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
}

// --- Compressor -------------------------------------------------------------------------

float Compressor::process(float* l, float* r, int n, float threshold_db, float ratio, float attack_ms,
                          float release_ms, float makeup_db) noexcept
{
    const float slope = 1.0f / std::max(1.0f, ratio) - 1.0f;  // <= 0
    const float knee = 6.0f;
    const auto coef = [this](float ms) {
        return static_cast<float>(std::exp(-1.0 / (std::max(0.05, static_cast<double>(ms)) * 0.001 * sr_)));
    };
    const float att = coef(attack_ms), rel = coef(release_ms);
    float deepest = 0.0f;
    float env = env_db_;
    for (int i = 0; i < n; ++i) {
        const float x = std::max(std::abs(l[i]), std::abs(r[i]));
        const float xdb = x > 1.0e-6f ? 20.0f * std::log10(x) : -120.0f;
        const float over = xdb - threshold_db;
        float target = 0.0f;
        if (2.0f * over >= knee) target = slope * over;
        else if (2.0f * over > -knee) target = slope * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee);
        const float c = target < env ? att : rel;
        env = target + c * (env - target);
        deepest = std::min(deepest, env);
        const float g = db_to_lin(env + makeup_db);
        l[i] *= g;
        r[i] *= g;
    }
    env_db_ = env;
    return deepest;
}

// --- Gate -------------------------------------------------------------------------------------

bool Gate::process(float* l, float* r, int n, float threshold_db, float range_db, float release_ms) noexcept
{
    const float threshold = db_to_lin(threshold_db);
    const float floor_gain = db_to_lin(-std::abs(range_db));
    const float env_release = static_cast<float>(std::exp(-1.0 / (0.010 * sr_)));
    const float open_coef = static_cast<float>(std::exp(-1.0 / (0.0005 * sr_)));
    const float close_coef = static_cast<float>(std::exp(-1.0 / (std::max(1.0, static_cast<double>(release_ms)) * 0.001 * sr_)));
    const int hold_samples = static_cast<int>(0.010 * sr_);
    float target = 1.0f;
    for (int i = 0; i < n; ++i) {
        const float x = std::max(std::abs(l[i]), std::abs(r[i]));
        env_ = x > env_ ? x : env_ * env_release;
        if (env_ >= threshold) hold_ = hold_samples;
        else if (hold_ > 0) --hold_;
        target = hold_ > 0 ? 1.0f : floor_gain;
        const float c = target > gain_ ? open_coef : close_coef;
        gain_ = target + c * (gain_ - target);
        l[i] *= gain_;
        r[i] *= gain_;
    }
    return target < 1.0f;
}

// --- ChannelProcessor ----------------------------------------------------------------------------

void ChannelProcessor::prepare(double sr)
{
    if (juce::exactlyEqual(sr, sr_)) return;
    sr_ = sr;
    comp_.prepare(sr);
    gate_.prepare(sr);
    for (Biquad* b : {&hpf_, &lpf_, &lf_, &lmf_, &hmf_, &hf_}) b->reset();
    comp_.reset();
    gate_.reset();
    valid_ = false;
}

namespace {
bool same_band(const project::EqBand& a, const project::EqBand& b) noexcept
{
    return juce::exactlyEqual(a.freq, b.freq) && juce::exactlyEqual(a.gain_db, b.gain_db) && juce::exactlyEqual(a.q, b.q) &&
           a.bell == b.bell;
}
}  // namespace

void ChannelProcessor::update(const project::ChannelStrip& s) noexcept
{
    if (valid_ && juce::exactlyEqual(s.hpf_hz, applied_.hpf_hz) && juce::exactlyEqual(s.lpf_hz, applied_.lpf_hz) && same_band(s.lf, applied_.lf) &&
        same_band(s.lmf, applied_.lmf) && same_band(s.hmf, applied_.hmf) && same_band(s.hf, applied_.hf))
        return;
    applied_ = s;
    valid_ = true;
    hpf_.set_highpass(sr_, s.hpf_hz, 0.7071);
    lpf_.set_lowpass(sr_, s.lpf_hz, 0.7071);
    if (s.lf.bell) lf_.set_peak(sr_, s.lf.freq, s.lf.q, s.lf.gain_db);
    else lf_.set_low_shelf(sr_, s.lf.freq, s.lf.q, s.lf.gain_db);
    lmf_.set_peak(sr_, s.lmf.freq, s.lmf.q, s.lmf.gain_db);
    hmf_.set_peak(sr_, s.hmf.freq, s.hmf.q, s.hmf.gain_db);
    if (s.hf.bell) hf_.set_peak(sr_, s.hf.freq, s.hf.q, s.hf.gain_db);
    else hf_.set_high_shelf(sr_, s.hf.freq, s.hf.q, s.hf.gain_db);
}

void ChannelProcessor::filters(float* l, float* r, int n, const project::ChannelStrip& s) noexcept
{
    if (s.hpf_on) {
        hpf_.process(l, 0, n);
        hpf_.process(r, 1, n);
    }
    if (s.lpf_on) {
        lpf_.process(l, 0, n);
        lpf_.process(r, 1, n);
    }
}

void ChannelProcessor::eq(float* l, float* r, int n, const project::ChannelStrip& s) noexcept
{
    if (!s.eq_on) return;
    const std::pair<const project::EqBand*, Biquad*> bands[] = {{&s.lf, &lf_}, {&s.lmf, &lmf_}, {&s.hmf, &hmf_}, {&s.hf, &hf_}};
    for (const auto& [band, filter] : bands)
        if (band->on && !juce::exactlyEqual(band->gain_db, 0.0)) {
            filter->process(l, 0, n);
            filter->process(r, 1, n);
        }
}

void ChannelProcessor::dynamics(float* l, float* r, int n, const project::ChannelStrip& s) noexcept
{
    gate_closed_ = false;
    if (s.gate_on)
        gate_closed_ = gate_.process(l, r, n, static_cast<float>(s.gate_threshold_db), static_cast<float>(s.gate_range_db),
                                     static_cast<float>(s.gate_release_ms));
    gr_db_ = 0.0f;
    if (s.comp_on)
        gr_db_ = comp_.process(l, r, n, static_cast<float>(s.comp_threshold_db), static_cast<float>(s.comp_ratio),
                               static_cast<float>(s.comp_attack_ms), static_cast<float>(s.comp_release_ms),
                               static_cast<float>(s.comp_makeup_db));
}

void ChannelProcessor::process(float* l, float* r, int n, const project::ChannelStrip& s) noexcept
{
    if (sr_ <= 0.0) return;
    juce::ScopedNoDenormals no_denormals;
    update(s);
    trim_target_ = db_to_lin(static_cast<float>(s.trim_db)) * (s.phase_invert ? -1.0f : 1.0f);
    ramp_gain(l, n, trim_, trim_target_);
    ramp_gain(r, n, trim_, trim_target_);
    trim_ = trim_target_;
    filters(l, r, n, s);
    if (s.dynamics_pre_eq) {
        dynamics(l, r, n, s);
        eq(l, r, n, s);
    } else {
        eq(l, r, n, s);
        dynamics(l, r, n, s);
    }
}

// --- Inserts ---------------------------------------------------------------------------------------

double insert_param(const project::InsertSlot& slot, const char* name, double fallback)
{
    auto it = slot.params.find(name);
    return it == slot.params.end() || !std::isfinite(it->second) ? fallback : it->second;
}

namespace {
// Automatable parameters per built-in kind, in automation-index order.
const char* const kEqParams[] = {"lf_gain", "lf_freq", "lmf_gain", "lmf_freq", "hmf_gain", "hmf_freq", "hf_gain",
                                 "hf_freq", "lf_q",    "lmf_q",   "hmf_q",    "hf_q",     "hpf_hz",   "lpf_hz"};
const char* const kCompParams[] = {"threshold_db", "ratio", "attack_ms", "release_ms", "makeup_db"};
const char* const kGateParams[] = {"threshold_db", "range_db", "release_ms"};
const char* const kReverbParams[] = {"room", "damping", "width", "mix"};
const char* const kDelayParams[] = {"time_ms", "feedback", "mix"};
const char* const kLimiterParams[] = {"ceiling_db", "release_ms"};
const char* const kSaturationParams[] = {"drive_db", "mix", "output_db"};

template <std::size_t N>
int index_in(const char* const (&names)[N], const std::string& name)
{
    for (std::size_t i = 0; i < N; ++i)
        if (name == names[i]) return static_cast<int>(i);
    return -1;
}
}  // namespace

int insert_automation_index(const std::string& kind, const std::string& name)
{
    if (kind == "eq") return index_in(kEqParams, name);
    if (kind == "compressor") return index_in(kCompParams, name);
    if (kind == "gate") return index_in(kGateParams, name);
    if (kind == "reverb") return index_in(kReverbParams, name);
    if (kind == "delay") return index_in(kDelayParams, name);
    if (kind == "limiter") return index_in(kLimiterParams, name);
    if (kind == "saturation") return index_in(kSaturationParams, name);
    return -1;
}

namespace {

/// eq, compressor and gate inserts reuse the channel strip's processing.
class StripInsert final : public InsertProcessor {
public:
    void prepare(double sr, int) override { channel_.prepare(sr); }
    void set_params(const project::InsertSlot& slot) override
    {
        project::ChannelStrip s;
        auto p = [&](const char* name, double d) { return insert_param(slot, name, d); };
        kind_ = slot.kind == "eq" ? 0 : slot.kind == "compressor" ? 1 : 2;
        if (slot.kind == "eq") {
            s.eq_on = true;
            s.lf = {true, p("lf_freq", 100), p("lf_gain", 0), p("lf_q", 0.7), p("lf_bell", 0) > 0.5};
            s.lmf = {true, p("lmf_freq", 400), p("lmf_gain", 0), p("lmf_q", 1.0), true};
            s.hmf = {true, p("hmf_freq", 2500), p("hmf_gain", 0), p("hmf_q", 1.0), true};
            s.hf = {true, p("hf_freq", 8000), p("hf_gain", 0), p("hf_q", 0.7), p("hf_bell", 0) > 0.5};
            s.hpf_hz = p("hpf_hz", 0);
            s.hpf_on = s.hpf_hz > 0;
            s.lpf_hz = p("lpf_hz", 0);
            s.lpf_on = s.lpf_hz > 0;
        } else if (slot.kind == "compressor") {
            s.comp_on = true;
            s.comp_threshold_db = p("threshold_db", -18);
            s.comp_ratio = p("ratio", 3);
            s.comp_attack_ms = p("attack_ms", 10);
            s.comp_release_ms = p("release_ms", 120);
            s.comp_makeup_db = p("makeup_db", 0);
        } else if (slot.kind == "gate") {
            s.gate_on = true;
            s.gate_threshold_db = p("threshold_db", -50);
            s.gate_range_db = p("range_db", 40);
            s.gate_release_ms = p("release_ms", 150);
        }
        box_.write(s);
    }
    void process(float* l, float* r, int n) noexcept override
    {
        if (automated_mask_ == 0) {
            channel_.process(l, r, n, box_.latest());
            return;
        }
        project::ChannelStrip s = box_.latest();
        auto a = [&](int i, double& field) {
            if (automated_mask_ & (1u << i)) field = automated_[static_cast<std::size_t>(i)];
        };
        if (kind_ == 0) {
            a(0, s.lf.gain_db), a(1, s.lf.freq), a(2, s.lmf.gain_db), a(3, s.lmf.freq), a(4, s.hmf.gain_db);
            a(5, s.hmf.freq), a(6, s.hf.gain_db), a(7, s.hf.freq), a(8, s.lf.q), a(9, s.lmf.q), a(10, s.hmf.q);
            a(11, s.hf.q), a(12, s.hpf_hz), a(13, s.lpf_hz);
            s.hpf_on = s.hpf_hz > 0;
            s.lpf_on = s.lpf_hz > 0;
        } else if (kind_ == 1) {
            a(0, s.comp_threshold_db), a(1, s.comp_ratio), a(2, s.comp_attack_ms), a(3, s.comp_release_ms);
            a(4, s.comp_makeup_db);
        } else {
            a(0, s.gate_threshold_db), a(1, s.gate_range_db), a(2, s.gate_release_ms);
        }
        channel_.process(l, r, n, s);
    }

private:
    Mailbox<project::ChannelStrip> box_;
    ChannelProcessor channel_;
    int kind_ = 0;  // 0 eq, 1 compressor, 2 gate
};

class ReverbInsert final : public InsertProcessor {
public:
    void prepare(double sr, int) override
    {
        if (juce::exactlyEqual(sr, sr_)) return;
        sr_ = sr;
        reverb_.setSampleRate(sr);
        reverb_.reset();
    }
    void set_params(const project::InsertSlot& slot) override
    {
        box_.write({static_cast<float>(insert_param(slot, "room", 0.82)), static_cast<float>(insert_param(slot, "damping", 0.4)),
                    static_cast<float>(insert_param(slot, "width", 1.0)), static_cast<float>(insert_param(slot, "mix", 1.0))});
    }
    void process(float* l, float* r, int n) noexcept override
    {
        const Raw& raw = box_.latest();
        juce::Reverb::Parameters p;
        p.roomSize = std::clamp(automated(0, raw.room), 0.0f, 1.0f);
        p.damping = std::clamp(automated(1, raw.damping), 0.0f, 1.0f);
        p.width = std::clamp(automated(2, raw.width), 0.0f, 1.0f);
        const float mix = std::clamp(automated(3, raw.mix), 0.0f, 1.0f);
        p.wetLevel = mix / 3.0f;            // juce::Reverb scales wet by 3: about unity at mix 1
        p.dryLevel = (1.0f - mix) * 0.5f;  // and dry by 2
        if (std::memcmp(&p, &applied_, sizeof(p)) != 0) {
            applied_ = p;
            reverb_.setParameters(p);
        }
        reverb_.processStereo(l, r, n);
    }

private:
    struct Raw {
        float room = 0.82f, damping = 0.4f, width = 1.0f, mix = 1.0f;
    };
    Mailbox<Raw> box_;
    juce::Reverb::Parameters applied_{-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    juce::Reverb reverb_;
    double sr_ = 0.0;
};

class DelayInsert final : public InsertProcessor {
public:
    void prepare(double sr, int max_block) override
    {
        if (juce::exactlyEqual(sr, sr_)) return;
        sr_ = sr;
        buffer_.setSize(2, static_cast<int>(sr * 2.1) + max_block + 4);
        buffer_.clear();
        write_ = 0;
        delay_ = -1.0f;
    }
    void set_params(const project::InsertSlot& slot) override
    {
        box_.write({static_cast<float>(std::clamp(insert_param(slot, "time_ms", 375.0), 1.0, 2000.0)),
                    static_cast<float>(std::clamp(insert_param(slot, "feedback", 0.35), 0.0, 0.95)),
                    static_cast<float>(std::clamp(insert_param(slot, "mix", 1.0), 0.0, 1.0))});
    }
    void process(float* l, float* r, int n) noexcept override
    {
        const int size = buffer_.getNumSamples();
        if (size == 0) return;
        Params p = box_.latest();
        if (automated_mask_ != 0) {
            p.time_ms = std::clamp(automated(0, p.time_ms), 1.0f, 2000.0f);
            p.feedback = std::clamp(automated(1, p.feedback), 0.0f, 0.95f);
            p.mix = std::clamp(automated(2, p.mix), 0.0f, 1.0f);
        }
        const float target = p.time_ms * 0.001f * static_cast<float>(sr_);
        if (delay_ < 0.0f) delay_ = target;
        const float smooth = 1.0f - std::exp(-1.0f / (0.05f * static_cast<float>(sr_)));  // 50 ms glide
        float* bl = buffer_.getWritePointer(0);
        float* br = buffer_.getWritePointer(1);
        for (int i = 0; i < n; ++i) {
            delay_ += (target - delay_) * smooth;
            float pos = static_cast<float>(write_) - delay_;
            while (pos < 0.0f) pos += static_cast<float>(size);
            const int i0 = static_cast<int>(pos) % size;
            const int i1 = (i0 + 1) % size;
            const float frac = pos - std::floor(pos);
            const float yl = bl[i0] + (bl[i1] - bl[i0]) * frac;
            const float yr = br[i0] + (br[i1] - br[i0]) * frac;
            bl[write_] = l[i] + yl * p.feedback;
            br[write_] = r[i] + yr * p.feedback;
            write_ = (write_ + 1) % size;
            l[i] = l[i] * (1.0f - p.mix) + yl * p.mix;
            r[i] = r[i] * (1.0f - p.mix) + yr * p.mix;
        }
    }

private:
    struct Params {
        float time_ms = 375.0f, feedback = 0.35f, mix = 1.0f;
    };
    Mailbox<Params> box_;
    juce::AudioBuffer<float> buffer_;
    int write_ = 0;
    float delay_ = -1.0f;
    double sr_ = 0.0;
};

class LimiterInsert final : public InsertProcessor {
public:
    void prepare(double sr, int) override { sr_ = sr; }
    void set_params(const project::InsertSlot& slot) override
    {
        box_.write({static_cast<float>(std::clamp(insert_param(slot, "ceiling_db", -0.3), -60.0, 0.0)),
                    static_cast<float>(std::clamp(insert_param(slot, "release_ms", 80.0), 1.0, 5000.0))});
    }
    void process(float* l, float* r, int n) noexcept override
    {
        const Params& p = box_.latest();
        const float ceiling = db_to_lin(std::clamp(automated(0, p.ceiling_db), -60.0f, 0.0f));
        const float release_ms = std::clamp(automated(1, p.release_ms), 1.0f, 5000.0f);
        const float rel = std::exp(-1.0f / (release_ms * 0.001f * static_cast<float>(sr_)));
        for (int i = 0; i < n; ++i) {
            const float peak = std::max(std::abs(l[i]), std::abs(r[i]));
            const float needed = peak > ceiling ? ceiling / peak : 1.0f;
            gain_ = std::min(1.0f + rel * (gain_ - 1.0f), needed);
            l[i] *= gain_;
            r[i] *= gain_;
        }
    }

private:
    struct Params {
        float ceiling_db = -0.3f, release_ms = 80.0f;
    };
    Mailbox<Params> box_;
    float gain_ = 1.0f;
    double sr_ = 48000.0;
};

class SaturationInsert final : public InsertProcessor {
public:
    void prepare(double, int) override {}
    void set_params(const project::InsertSlot& slot) override
    {
        box_.write({static_cast<float>(insert_param(slot, "drive_db", 6.0)), static_cast<float>(insert_param(slot, "mix", 1.0)),
                    static_cast<float>(insert_param(slot, "output_db", 0.0))});
    }
    void process(float* l, float* r, int n) noexcept override
    {
        const Params& raw = box_.latest();
        const float drive = db_to_lin(std::clamp(automated(0, raw.drive_db), 0.0f, 36.0f));
        const float mix = std::clamp(automated(1, raw.mix), 0.0f, 1.0f);
        const float output = db_to_lin(std::clamp(automated(2, raw.output_db), -24.0f, 24.0f));
        for (float* d : {l, r})
            for (int i = 0; i < n; ++i) d[i] = (d[i] * (1.0f - mix) + std::tanh(d[i] * drive) * mix) * output;
    }

private:
    struct Params {
        float drive_db = 6.0f, mix = 1.0f, output_db = 0.0f;
    };
    Mailbox<Params> box_;
};

class PluginInsert final : public InsertProcessor {
public:
    explicit PluginInsert(std::unique_ptr<juce::AudioPluginInstance> instance) : instance_(std::move(instance)) {}
    ~PluginInsert() override
    {
        if (instance_) instance_->releaseResources();
    }
    void prepare(double sr, int max_block) override
    {
        if (juce::exactlyEqual(sr, sr_) && max_block == block_) return;
        sr_ = sr;
        block_ = max_block;
        instance_->releaseResources();
        instance_->setRateAndBufferSizeDetails(sr, max_block);
        instance_->prepareToPlay(sr, max_block);
        const int channels = juce::jmax(2, instance_->getTotalNumInputChannels(), instance_->getTotalNumOutputChannels());
        buffer_.setSize(channels, juce::jmax(1, max_block));
        midi_.ensureSize(256);
    }
    void set_params(const project::InsertSlot&) override {}
    void process(float* l, float* r, int n) noexcept override { run(l, r, n, false); }
    void process_bypassed(float* l, float* r, int n) noexcept override { run(l, r, n, true); }
    int latency_samples() const noexcept override { return instance_->getLatencySamples(); }
    void set_watch(std::unique_ptr<ParameterWatch> w) override { watch_ = std::move(w); }
    bool has_watch() const noexcept override { return watch_ != nullptr; }
    juce::AudioPluginInstance* plugin() noexcept override { return instance_.get(); }

private:
    void run(float* l, float* r, int n, bool bypassed) noexcept
    {
        if (n > buffer_.getNumSamples()) return;
        juce::AudioBuffer<float> view(buffer_.getArrayOfWritePointers(), buffer_.getNumChannels(), n);
        view.clear();
        const int ins = instance_->getTotalNumInputChannels();
        if (ins == 1) {
            view.copyFrom(0, 0, l, n, 0.5f);
            view.addFrom(0, 0, r, n, 0.5f);
        } else if (ins >= 2) {
            view.copyFrom(0, 0, l, n);
            view.copyFrom(1, 0, r, n);
        }
        midi_.clear();
        if (bypassed) instance_->processBlockBypassed(view, midi_);
        else instance_->processBlock(view, midi_);
        const int outs = instance_->getTotalNumOutputChannels();
        if (outs <= 0) return;
        juce::FloatVectorOperations::copy(l, view.getReadPointer(0), n);
        juce::FloatVectorOperations::copy(r, view.getReadPointer(outs == 1 ? 0 : 1), n);
    }

    std::unique_ptr<juce::AudioPluginInstance> instance_;
    std::unique_ptr<ParameterWatch> watch_;  // destroyed before the instance
    juce::AudioBuffer<float> buffer_;
    juce::MidiBuffer midi_;
    double sr_ = 0.0;
    int block_ = 0;
};

/// A plug-in that could not be loaded: audio passes through, status says why.
class MissingInsert final : public InsertProcessor {
public:
    explicit MissingInsert(juce::String why) : why_(std::move(why)) {}
    void prepare(double, int) override {}
    void set_params(const project::InsertSlot&) override {}
    void process(float*, float*, int) noexcept override {}
    juce::String status() const override { return why_; }

private:
    juce::String why_;
};

}  // namespace

bool is_known_insert_kind(const std::string& k) noexcept
{
    return k == "eq" || k == "compressor" || k == "gate" || k == "reverb" || k == "delay" || k == "limiter" ||
           k == "saturation" || k == "plugin";
}

std::unique_ptr<InsertProcessor> create_insert(const project::InsertSlot& slot, PluginHost& host, double sr, int max_block)
{
    std::unique_ptr<InsertProcessor> p;
    const auto& k = slot.kind;
    if (k == "eq" || k == "compressor" || k == "gate") p = std::make_unique<StripInsert>();
    else if (k == "reverb") p = std::make_unique<ReverbInsert>();
    else if (k == "delay") p = std::make_unique<DelayInsert>();
    else if (k == "limiter") p = std::make_unique<LimiterInsert>();
    else if (k == "saturation") p = std::make_unique<SaturationInsert>();
    else if (k == "plugin") {
        if (slot.plugin.empty()) {
            p = std::make_unique<MissingInsert>("No plug-in chosen");
        } else {
            juce::String error;
            auto instance = host.create(slot.plugin, sr, max_block, error);
            if (instance) p = std::make_unique<PluginInsert>(std::move(instance));
            else p = std::make_unique<MissingInsert>("Missing: " + juce::String(slot.plugin.name) + " (" + error + ") - bypassed");
        }
    }
    if (p) {
        p->prepare(sr, max_block);
        p->set_params(slot);
    }
    return p;
}

}  // namespace vcc::desktop
