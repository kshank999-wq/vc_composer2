// VC Dynamic EQ (insert kind "dyneq"): a digital parallel dynamic equaliser.
//
// Four bands, each a low shelf, bell or high shelf, run in parallel: each
// band's filter takes its share of the input, and the output is the input
// plus every band's share times (gain - 1). The bands never feed each
// other, so moving one leaves the others as they were. A band switched to
// dynamic listens to its own share: above the threshold it turns its gain
// down by the ratio (up to 30 dB), with its own attack and release. A high-
// and a low-pass filter (12/24/36/48 dB per octave) follow, then the dry mix
// and the output level.
//
// Parameters (InsertSlot::params; missing names take these defaults):
//   b1_..b4_ on 1, type (0 low shelf, 1 bell, 2 high shelf) 1,
//            freq (b1 120, b2 600, b3 2500, b4 8000), q 1, gain_db 0 (-24..24),
//            dyn 0, threshold_db -30 (-80..0), ratio 2 (1..20),
//            attack_ms 10, release_ms 150
//   hp_on 0, hp_freq 40, hp_slope 1 (0..3 = 12/24/36/48 dB/oct),
//   lp_on 0, lp_freq 18000, lp_slope 1,
//   dry_mix 0 (0..1), out_db 0 (-24..24), solo 0 (1..4: hear that band alone),
//   auto_gain 0 (1: the output level follows the input's, whatever the EQ does),
//   ext_sc 0 (1: dynamic bands listen to the slot's sidechain key, another
//            track or bus, filtered to each band, instead of the signal)
#pragma once

#include <array>
#include <atomic>
#include <complex>
#include <string>
#include <vector>

#include "audio/ChannelDsp.h"

namespace vcc::desktop {

struct DynEqBand {
    bool on = true;
    int type = 1;  ///< 0 low shelf, 1 bell, 2 high shelf.
    double freq = 1000.0, q = 1.0, gain_db = 0.0;
    bool dyn = false;
    double threshold_db = -30.0, ratio = 2.0, attack_ms = 10.0, release_ms = 150.0;
};

struct DynEqSettings {
    static constexpr int kBands = 4;
    std::array<DynEqBand, kBands> bands{};
    bool hp_on = false, lp_on = false;
    double hp_freq = 40.0, lp_freq = 18000.0;
    int hp_slope = 1, lp_slope = 1;  ///< 0..3: 12, 24, 36, 48 dB/oct.
    double dry_mix = 0.0, out_db = 0.0;
    int solo = 0;  ///< 1..4: that band alone.
    bool auto_gain = false;  ///< Match the output's loudness to the input's.
    bool ext_sc = false;     ///< Dynamic bands keyed by the sidechain.

    static DynEqSettings from_slot(const project::InsertSlot& slot);
    /// The magnitude (dB) at `freq`; `band_gain_db` overrides each band's
    /// gain (the dynamic gain now playing), or null for the set gains.
    double response_db(double freq, double sr, const float* band_gain_db = nullptr) const;
    /// One band's own curve (dB) at `freq`, for its shaded area.
    double band_response_db(int band, double freq, double sr, double gain_db) const;
};

/// The parameter names, in automation-index order (51 of them).
const std::vector<std::string>& dyneq_param_names();
/// The default for one parameter name ("b2_freq" -> 600).
double dyneq_default(const std::string& name);

/// The processor. The rack reads `live_gain_db` (each band's gain now) and
/// the analyser's samples while it plays.
class DynamicEqInsert final : public InsertProcessor {
public:
    void prepare(double sr, int max_block) override;
    void set_params(const project::InsertSlot& slot) override;
    void process(float* l, float* r, int n) noexcept override;

    std::array<std::atomic<float>, DynEqSettings::kBands> live_gain_db{};
    /// What auto gain is adding now (dB).
    std::atomic<float> live_auto_gain_db{0.0f};
    /// Whether a sidechain key reached the last block.
    std::atomic<bool> live_keyed{false};
    /// The last `n` samples (mono, newest last) of the output, or of the
    /// input with `input`, for the real-time analyser. Message thread;
    /// false while nothing has played.
    bool read_spectrum_samples(float* out, int n, bool input = false) const;
    double sample_rate() const noexcept { return sr_; }

private:
    void configure(const DynEqSettings& s) noexcept;
    DynEqSettings automate_settings(const DynEqSettings& s) const noexcept;

    Mailbox<DynEqSettings> box_;
    DynEqSettings applied_{};
    bool configured_ = false;
    double sr_ = 48000.0;
    std::array<Biquad, DynEqSettings::kBands> band_filter_{}, key_filter_{};
    std::vector<float> key_l_buf_, key_r_buf_;
    double in_ms_ = 0.0, out_ms_ = 0.0;
    float auto_gain_ = 1.0f;
    std::array<float, DynEqSettings::kBands> env_db_{};
    std::array<float, DynEqSettings::kBands> gain_{};  ///< Linear gain now (smoothed).
    std::array<Biquad, 4> hp_{}, lp_{};
    std::vector<float> band_l_, band_r_, dry_l_, dry_r_;
    static constexpr int kRing = 8192;
    std::array<float, kRing> ring_{}, ring_in_{};
    std::atomic<int> ring_pos_{0};
    std::atomic<bool> heard_{false};
};

}  // namespace vcc::desktop
