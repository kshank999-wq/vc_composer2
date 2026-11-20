#include "audio/DynamicEq.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vcc::desktop {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxCutDb = 30.0;

struct Coefs {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

double clamp_f(double sr, double f) { return std::clamp(f, 10.0, sr * 0.49); }

Coefs norm(double b0, double b1, double b2, double a0, double a1, double a2)
{
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

Coefs lowpass(double sr, double f, double q)
{
    const double w = 2 * kPi * clamp_f(sr, f) / sr, c = std::cos(w), al = std::sin(w) / (2 * std::max(0.1, q));
    return norm((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}
Coefs highpass(double sr, double f, double q)
{
    const double w = 2 * kPi * clamp_f(sr, f) / sr, c = std::cos(w), al = std::sin(w) / (2 * std::max(0.1, q));
    return norm((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}
/// Band-pass with 0 dB at the centre.
Coefs bandpass(double sr, double f, double q)
{
    const double w = 2 * kPi * clamp_f(sr, f) / sr, c = std::cos(w), al = std::sin(w) / (2 * std::max(0.1, q));
    return norm(al, 0, -al, 1 + al, -2 * c, 1 - al);
}

/// A band's own filter: its share of the signal. Shelves take everything
/// below (or above) the corner, bells a band round the centre.
Coefs band_coefs(const DynEqBand& b, double sr)
{
    const double q = std::clamp(b.q, 0.1, 18.0);
    if (b.type == 0) return lowpass(sr, b.freq, q * 0.7071);
    if (b.type == 2) return highpass(sr, b.freq, q * 0.7071);
    return bandpass(sr, b.freq, q);
}

std::complex<double> eval(const Coefs& c, double sr, double f)
{
    const double w = 2 * kPi * f / sr;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2 * w);
    return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
}

void apply(Biquad& bq, const Coefs& c) { bq.set_normalized(c.b0, c.b1, c.b2, c.a1, c.a2); }

/// Field setters in automation-index order.
void set_field(DynEqSettings& s, int index, double v)
{
    if (index < 0) return;
    if (index < 40) {
        auto& b = s.bands[static_cast<std::size_t>(index / 10)];
        switch (index % 10) {
            case 0: b.on = v >= 0.5; break;
            case 1: b.type = std::clamp(static_cast<int>(std::lround(v)), 0, 2); break;
            case 2: b.freq = std::clamp(v, 20.0, 20000.0); break;
            case 3: b.q = std::clamp(v, 0.1, 18.0); break;
            case 4: b.gain_db = std::clamp(v, -24.0, 24.0); break;
            case 5: b.dyn = v >= 0.5; break;
            case 6: b.threshold_db = std::clamp(v, -80.0, 0.0); break;
            case 7: b.ratio = std::clamp(v, 1.0, 20.0); break;
            case 8: b.attack_ms = std::clamp(v, 0.1, 500.0); break;
            case 9: b.release_ms = std::clamp(v, 5.0, 5000.0); break;
            default: break;
        }
        return;
    }
    switch (index) {
        case 40: s.hp_on = v >= 0.5; break;
        case 41: s.hp_freq = std::clamp(v, 10.0, 2000.0); break;
        case 42: s.hp_slope = std::clamp(static_cast<int>(std::lround(v)), 0, 3); break;
        case 43: s.lp_on = v >= 0.5; break;
        case 44: s.lp_freq = std::clamp(v, 500.0, 22000.0); break;
        case 45: s.lp_slope = std::clamp(static_cast<int>(std::lround(v)), 0, 3); break;
        case 46: s.dry_mix = std::clamp(v, 0.0, 1.0); break;
        case 47: s.out_db = std::clamp(v, -24.0, 24.0); break;
        case 48: s.solo = std::clamp(static_cast<int>(std::lround(v)), 0, 4); break;
        case 49: s.auto_gain = v >= 0.5; break;
        case 50: s.ext_sc = v >= 0.5; break;
        default: break;
    }
}

}  // namespace

const std::vector<std::string>& dyneq_param_names()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (int b = 1; b <= DynEqSettings::kBands; ++b)
            for (const char* f : {"on", "type", "freq", "q", "gain_db", "dyn", "threshold_db", "ratio", "attack_ms", "release_ms"})
                n.push_back("b" + std::to_string(b) + "_" + f);
        for (const char* f : {"hp_on", "hp_freq", "hp_slope", "lp_on", "lp_freq", "lp_slope", "dry_mix", "out_db", "solo",
                              "auto_gain", "ext_sc"})
            n.emplace_back(f);
        return n;
    }();
    return names;
}

double dyneq_default(const std::string& name)
{
    static const double freqs[] = {120.0, 600.0, 2500.0, 8000.0};
    if (name.size() > 3 && name[0] == 'b' && name[2] == '_') {
        const int b = name[1] - '1';
        const std::string f = name.substr(3);
        if (f == "on") return 1;
        if (f == "type") return 1;
        if (f == "freq") return b >= 0 && b < 4 ? freqs[b] : 1000.0;
        if (f == "q") return 1;
        if (f == "threshold_db") return -30;
        if (f == "ratio") return 2;
        if (f == "attack_ms") return 10;
        if (f == "release_ms") return 150;
        return 0;
    }
    if (name == "hp_freq") return 40;
    if (name == "lp_freq") return 18000;
    if (name == "hp_slope" || name == "lp_slope") return 1;
    return 0;
}

DynEqSettings DynEqSettings::from_slot(const project::InsertSlot& slot)
{
    DynEqSettings s;
    const auto& names = dyneq_param_names();
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto it = slot.params.find(names[i]);
        const double v = it != slot.params.end() && std::isfinite(it->second) ? it->second : dyneq_default(names[i]);
        set_field(s, static_cast<int>(i), v);
    }
    return s;
}

double DynEqSettings::band_response_db(int band, double freq, double sr, double gain_db) const
{
    const auto& b = bands[static_cast<std::size_t>(band)];
    const double g = std::pow(10.0, gain_db / 20.0);
    const auto h = 1.0 + (g - 1.0) * eval(band_coefs(b, sr), sr, freq);
    return 20.0 * std::log10(std::max(1e-9, std::abs(h)));
}

double DynEqSettings::response_db(double freq, double sr, const float* band_gain_db) const
{
    std::complex<double> h = 1.0;
    if (solo > 0) {
        const auto& b = bands[static_cast<std::size_t>(solo - 1)];
        const double gdb = band_gain_db ? band_gain_db[solo - 1] : b.gain_db;
        h = std::pow(10.0, gdb / 20.0) * eval(band_coefs(b, sr), sr, freq);
    } else {
        for (int i = 0; i < kBands; ++i) {
            const auto& b = bands[static_cast<std::size_t>(i)];
            if (!b.on) continue;
            const double gdb = band_gain_db ? band_gain_db[i] : b.gain_db;
            h += (std::pow(10.0, gdb / 20.0) - 1.0) * eval(band_coefs(b, sr), sr, freq);
        }
    }
    const double butter[4][4] = {{0.7071}, {0.5412, 1.3066}, {0.5176, 0.7071, 1.9319}, {0.5098, 0.6013, 0.9000, 2.5629}};
    if (hp_on)
        for (int k = 0; k <= hp_slope; ++k) h *= eval(highpass(sr, hp_freq, butter[hp_slope][k]), sr, freq);
    if (lp_on)
        for (int k = 0; k <= lp_slope; ++k) h *= eval(lowpass(sr, lp_freq, butter[lp_slope][k]), sr, freq);
    // The dry mix and the output level.
    const double mag = std::abs(h * (1.0 - dry_mix) + dry_mix);
    return 20.0 * std::log10(std::max(1e-9, mag)) + out_db;
}

// --- The processor ---------------------------------------------------------------------------

void DynamicEqInsert::prepare(double sr, int max_block)
{
    if (sr != sr_) configured_ = false;
    sr_ = sr;
    const auto n = static_cast<std::size_t>(std::max(1, max_block));
    band_l_.assign(n, 0.0f);
    band_r_.assign(n, 0.0f);
    dry_l_.assign(n, 0.0f);
    dry_r_.assign(n, 0.0f);
    key_l_buf_.assign(n, 0.0f);
    key_r_buf_.assign(n, 0.0f);
    for (auto& g : gain_) g = 1.0f;
    for (auto& e : env_db_) e = -120.0f;
}

void DynamicEqInsert::set_params(const project::InsertSlot& slot) { box_.write(DynEqSettings::from_slot(slot)); }

DynEqSettings DynamicEqInsert::automate_settings(const DynEqSettings& s) const noexcept
{
    if (automated_mask_ == 0) return s;
    DynEqSettings a = s;
    for (int i = 0; i < kMaxAutomated; ++i)
        if (automated_mask_ & (std::uint64_t{1} << i)) set_field(a, i, automated_[static_cast<std::size_t>(i)]);
    return a;
}

void DynamicEqInsert::configure(const DynEqSettings& s) noexcept
{
    for (int i = 0; i < DynEqSettings::kBands; ++i) {
        const auto& b = s.bands[static_cast<std::size_t>(i)];
        const auto& was = applied_.bands[static_cast<std::size_t>(i)];
        if (!configured_ || b.type != was.type || b.freq != was.freq || b.q != was.q) {
            apply(band_filter_[static_cast<std::size_t>(i)], band_coefs(b, sr_));
            apply(key_filter_[static_cast<std::size_t>(i)], band_coefs(b, sr_));
        }
    }
    const double butter[4][4] = {{0.7071}, {0.5412, 1.3066}, {0.5176, 0.7071, 1.9319}, {0.5098, 0.6013, 0.9000, 2.5629}};
    if (!configured_ || s.hp_freq != applied_.hp_freq || s.hp_slope != applied_.hp_slope)
        for (int k = 0; k <= s.hp_slope; ++k) apply(hp_[static_cast<std::size_t>(k)], highpass(sr_, s.hp_freq, butter[s.hp_slope][k]));
    if (!configured_ || s.lp_freq != applied_.lp_freq || s.lp_slope != applied_.lp_slope)
        for (int k = 0; k <= s.lp_slope; ++k) apply(lp_[static_cast<std::size_t>(k)], lowpass(sr_, s.lp_freq, butter[s.lp_slope][k]));
    if (configured_ && s.hp_on && !applied_.hp_on)
        for (auto& f : hp_) f.reset();
    if (configured_ && s.lp_on && !applied_.lp_on)
        for (auto& f : lp_) f.reset();
    applied_ = s;
    configured_ = true;
}

void DynamicEqInsert::process(float* l, float* r, int n) noexcept
{
    if (n <= 0) return;
    if (n > static_cast<int>(dry_l_.size())) {
        // Larger than prepared (should not happen): process in pieces.
        const int half = n / 2;
        process(l, r, half);
        process(l + half, r + half, n - half);
        return;
    }
    const DynEqSettings s = automate_settings(box_.latest());
    configure(s);
    std::memcpy(dry_l_.data(), l, sizeof(float) * static_cast<std::size_t>(n));
    std::memcpy(dry_r_.data(), r, sizeof(float) * static_cast<std::size_t>(n));
    if (s.solo > 0) {
        std::fill(l, l + n, 0.0f);
        std::fill(r, r + n, 0.0f);
    }
    const float smooth = 1.0f - std::exp(-1.0f / (0.004f * static_cast<float>(sr_)));  // 4 ms gain glide
    const bool keyed = s.ext_sc && key_l_ != nullptr;
    live_keyed.store(key_l_ != nullptr, std::memory_order_relaxed);
    for (int bi = 0; bi < DynEqSettings::kBands; ++bi) {
        const auto& b = s.bands[static_cast<std::size_t>(bi)];
        const auto ub = static_cast<std::size_t>(bi);
        const bool soloed = s.solo == bi + 1;
        if (!b.on && !soloed) {
            live_gain_db[ub].store(0.0f, std::memory_order_relaxed);
            gain_[ub] = 1.0f;
            continue;
        }
        std::memcpy(band_l_.data(), dry_l_.data(), sizeof(float) * static_cast<std::size_t>(n));
        std::memcpy(band_r_.data(), dry_r_.data(), sizeof(float) * static_cast<std::size_t>(n));
        auto& f = band_filter_[ub];
        f.process(band_l_.data(), 0, n);
        f.process(band_r_.data(), 1, n);
        // The detector: the band's share of the key (sidechain), or of the signal.
        const float* det_l = band_l_.data();
        const float* det_r = band_r_.data();
        if (keyed && b.dyn) {
            std::memcpy(key_l_buf_.data(), key_l_, sizeof(float) * static_cast<std::size_t>(n));
            std::memcpy(key_r_buf_.data(), key_r_ ? key_r_ : key_l_, sizeof(float) * static_cast<std::size_t>(n));
            key_filter_[ub].process(key_l_buf_.data(), 0, n);
            key_filter_[ub].process(key_r_buf_.data(), 1, n);
            det_l = key_l_buf_.data();
            det_r = key_r_buf_.data();
        }
        const float att = std::exp(-1.0f / (static_cast<float>(b.attack_ms) * 0.001f * static_cast<float>(sr_)));
        const float rel = std::exp(-1.0f / (static_cast<float>(b.release_ms) * 0.001f * static_cast<float>(sr_)));
        const float slope = 1.0f - 1.0f / static_cast<float>(std::max(1.0, b.ratio));
        float env = env_db_[ub], g = gain_[ub], target_db = static_cast<float>(b.gain_db);
        for (int i = 0; i < n; ++i) {
            if (b.dyn) {
                const float peak = std::max(std::abs(det_l[i]), std::abs(det_r[i]));
                const float lvl = 20.0f * std::log10(peak + 1e-9f);
                env = lvl > env ? att * env + (1.0f - att) * lvl : rel * env + (1.0f - rel) * lvl;
                const float over = std::max(0.0f, env - static_cast<float>(b.threshold_db));
                target_db = static_cast<float>(b.gain_db) - std::min(static_cast<float>(kMaxCutDb), over * slope);
            }
            const float target = std::exp(target_db * 0.11512925464970229f);
            g += (target - g) * smooth;
            const auto ui = static_cast<std::size_t>(i);
            if (s.solo > 0) {
                if (soloed) {
                    l[i] += g * band_l_[ui];
                    r[i] += g * band_r_[ui];
                }
            } else {
                l[i] += (g - 1.0f) * band_l_[ui];
                r[i] += (g - 1.0f) * band_r_[ui];
            }
        }
        env_db_[ub] = env;
        gain_[ub] = g;
        live_gain_db[ub].store(20.0f * std::log10(std::max(1e-6f, g)), std::memory_order_relaxed);
    }
    if (s.hp_on)
        for (int k = 0; k <= s.hp_slope; ++k) {
            hp_[static_cast<std::size_t>(k)].process(l, 0, n);
            hp_[static_cast<std::size_t>(k)].process(r, 1, n);
        }
    if (s.lp_on)
        for (int k = 0; k <= s.lp_slope; ++k) {
            lp_[static_cast<std::size_t>(k)].process(l, 0, n);
            lp_[static_cast<std::size_t>(k)].process(r, 1, n);
        }
    const float mix = static_cast<float>(s.dry_mix), out = std::exp(static_cast<float>(s.out_db) * 0.11512925464970229f);
    // Auto gain: the processed level is held to the input's (mean square
    // over about a second and a half, within 18 dB), so a boost or a cut
    // can be judged by its colour, not its loudness.
    double in_sum = 0.0, out_sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        l[i] = l[i] * (1.0f - mix) + dry_l_[ui] * mix;
        r[i] = r[i] * (1.0f - mix) + dry_r_[ui] * mix;
        in_sum += static_cast<double>(dry_l_[ui]) * dry_l_[ui] + static_cast<double>(dry_r_[ui]) * dry_r_[ui];
        out_sum += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
    }
    const double k = 1.0 - std::exp(-static_cast<double>(n) / (1.5 * sr_));
    in_ms_ += (in_sum / (2.0 * n) - in_ms_) * k;
    out_ms_ += (out_sum / (2.0 * n) - out_ms_) * k;
    float target = 1.0f;
    if (s.auto_gain && in_ms_ > 1e-8 && out_ms_ > 1e-12)
        target = static_cast<float>(std::clamp(std::sqrt(in_ms_ / out_ms_), 0.125, 8.0));
    const float from = auto_gain_;
    auto_gain_ = target == 1.0f && !s.auto_gain ? 1.0f : from + (target - from) * 0.2f;
    live_auto_gain_db.store(20.0f * std::log10(auto_gain_), std::memory_order_relaxed);
    int pos = ring_pos_.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        const float g = (from + (auto_gain_ - from) * static_cast<float>(i + 1) / static_cast<float>(n)) * out;
        l[i] *= g;
        r[i] *= g;
        ring_[static_cast<std::size_t>(pos)] = 0.5f * (l[i] + r[i]);
        ring_in_[static_cast<std::size_t>(pos)] = 0.5f * (dry_l_[static_cast<std::size_t>(i)] + dry_r_[static_cast<std::size_t>(i)]);
        pos = (pos + 1) & (kRing - 1);
    }
    ring_pos_.store(pos, std::memory_order_release);
    heard_.store(true, std::memory_order_relaxed);
}

bool DynamicEqInsert::read_spectrum_samples(float* out, int n, bool input) const
{
    if (!heard_.load(std::memory_order_relaxed) || n > kRing) return false;
    const int end = ring_pos_.load(std::memory_order_acquire);
    const auto& ring = input ? ring_in_ : ring_;
    for (int i = 0; i < n; ++i) out[i] = ring[static_cast<std::size_t>((end - n + i + kRing) & (kRing - 1))];
    return true;
}

}  // namespace vcc::desktop
