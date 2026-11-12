#include "audio/AutomationRt.h"

#include <algorithm>
#include <cmath>

#include "vcc/project/Automation.h"

namespace vcc::desktop {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::uint8_t shape_code(const std::string& s) { return s == "step" ? 1 : s == "smooth" ? 2 : 0; }

std::int64_t at(const TempoMap& tempo, double sr, Ticks t)
{
    return static_cast<std::int64_t>(std::llround(tempo.seconds_at(std::max<Ticks>(0, t)) * sr));
}

}  // namespace

// --- Curves ------------------------------------------------------------------------------

Curve make_curve(const project::AutomationLane& lane, const TempoMap& tempo, double sr, bool toggle)
{
    Curve out;
    const auto& pts = lane.points;
    out.reserve(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const auto& p = pts[i];
        const std::uint8_t shape = toggle ? 1 : shape_code(p.shape);
        out.push_back({at(tempo, sr, p.tick), p.value, shape});
        if (shape == 1 || i + 1 >= pts.size()) continue;
        // A tempo change inside the segment bends it in samples: follow the
        // lane in ticks with short linear pieces.
        const auto& q = pts[i + 1];
        const Ticks span = q.tick - p.tick;
        if (span < 2) continue;
        const double s0 = tempo.seconds_at(p.tick), s1 = tempo.seconds_at(q.tick);
        const double mid = tempo.seconds_at(p.tick + span / 2);
        const double linear = s0 + (s1 - s0) * static_cast<double>(span / 2) / static_cast<double>(span);
        if (std::abs(mid - linear) * sr < 1.0) continue;
        out.back().shape = 0;
        const int pieces = static_cast<int>(std::clamp<Ticks>(span / (kTicksPerQuarter / 8), 8, 512));
        project::AutomationLane one;
        one.points = {p, q};
        for (int k = 1; k < pieces; ++k) {
            const Ticks t = p.tick + span * k / pieces;
            out.push_back({at(tempo, sr, t), project::value_at(one, t).value_or(p.value), 0});
        }
    }
    // Equal sample positions (dense points at a low rate): keep the last.
    Curve merged;
    merged.reserve(out.size());
    for (const auto& c : out) {
        if (!merged.empty() && merged.back().sample >= c.sample) merged.back() = {merged.back().sample, c.value, c.shape};
        else merged.push_back(c);
    }
    return merged;
}

Curve make_mute_gain_curve(const project::AutomationLane& lane, const TempoMap& tempo, double sr, std::int64_t fade)
{
    Curve out;
    if (lane.points.empty()) return out;
    double gain = lane.points.front().value >= 0.5 ? 0.0 : 1.0;
    out.push_back({at(tempo, sr, lane.points.front().tick), gain, 0});
    for (const auto& p : lane.points) {
        const double g = p.value >= 0.5 ? 0.0 : 1.0;
        if (juce::exactlyEqual(g, gain)) continue;
        const std::int64_t s = at(tempo, sr, p.tick);
        if (g < gain) {
            // Closing: the fade ends on the point, so it is silent from there.
            out.push_back({std::max(out.back().sample, s - fade), gain, 0});
            out.push_back({std::max(out.back().sample, s), g, 0});
        } else {
            out.push_back({std::max(out.back().sample, s), gain, 0});
            out.push_back({std::max(out.back().sample, s + fade), g, 0});
        }
        gain = g;
    }
    return out;
}

namespace {

inline double segment_value(const CurvePoint& a, const CurvePoint& b, std::int64_t t) noexcept
{
    if (a.shape == 1 || b.sample <= a.sample) return a.value;
    double x = static_cast<double>(t - a.sample) / static_cast<double>(b.sample - a.sample);
    if (a.shape == 2) x = 0.5 - 0.5 * std::cos(kPi * x);
    return a.value + (b.value - a.value) * x;
}

/// Index of the first point after `t`.
inline std::size_t after(const Curve& c, std::int64_t t) noexcept
{
    return static_cast<std::size_t>(
        std::upper_bound(c.begin(), c.end(), t, [](std::int64_t s, const CurvePoint& p) { return s < p.sample; }) -
        c.begin());
}

}  // namespace

double curve_value(const Curve& c, std::int64_t t) noexcept
{
    if (c.empty()) return 0.0;
    const std::size_t i = after(c, t);
    if (i == 0) return c.front().value;
    if (i >= c.size()) return c.back().value;
    return segment_value(c[i - 1], c[i], t);
}

void apply_curve_gain(const Curve& c, std::int64_t from, float* l, float* r, int n) noexcept
{
    if (c.empty() || n <= 0) return;
    std::size_t i = after(c, from);
    // Constant over the whole block (the usual case): one multiply or none.
    const bool constant = i >= c.size() || (c[i].sample >= from + n && (i == 0 || c[i - 1].shape == 1 ||
                                                                         juce::exactlyEqual(c[i - 1].value, c[i].value)));
    if (constant) {
        const auto g = static_cast<float>(i == 0 ? c.front().value : c[i - 1].value);
        if (juce::exactlyEqual(g, 1.0f)) return;
        juce::FloatVectorOperations::multiply(l, g, n);
        juce::FloatVectorOperations::multiply(r, g, n);
        return;
    }
    for (int k = 0; k < n; ++k) {
        const std::int64_t t = from + k;
        while (i < c.size() && c[i].sample <= t) ++i;
        const double v = i == 0 ? c.front().value : i >= c.size() ? c.back().value : segment_value(c[i - 1], c[i], t);
        const auto g = static_cast<float>(v);
        l[k] *= g;
        r[k] *= g;
    }
}

// --- Channel strip fields ------------------------------------------------------------------

namespace {
const char* const kStripFields[] = {
    "strip.trim_db",          "strip.phase_invert",      "strip.hpf_on",          "strip.hpf_hz",
    "strip.lpf_on",           "strip.lpf_hz",            "strip.eq_on",           "strip.lf.gain_db",
    "strip.lf.freq",          "strip.lf.q",              "strip.lmf.gain_db",     "strip.lmf.freq",
    "strip.lmf.q",            "strip.hmf.gain_db",       "strip.hmf.freq",        "strip.hmf.q",
    "strip.hf.gain_db",       "strip.hf.freq",           "strip.hf.q",            "strip.comp_on",
    "strip.comp_threshold_db", "strip.comp_ratio",       "strip.comp_attack_ms",  "strip.comp_release_ms",
    "strip.comp_makeup_db",   "strip.gate_on",           "strip.gate_threshold_db", "strip.gate_range_db",
    "strip.gate_release_ms",
};
}  // namespace

int strip_field_index(const std::string& param)
{
    for (int i = 0; i < static_cast<int>(std::size(kStripFields)); ++i)
        if (param == kStripFields[i]) return i;
    return -1;
}

void set_strip_field(project::ChannelStrip& s, int index, double v) noexcept
{
    const bool on = v >= 0.5;
    switch (index) {
        case 0: s.trim_db = v; break;
        case 1: s.phase_invert = on; break;
        case 2: s.hpf_on = on; break;
        case 3: s.hpf_hz = v; break;
        case 4: s.lpf_on = on; break;
        case 5: s.lpf_hz = v; break;
        case 6: s.eq_on = on; break;
        case 7: s.lf.gain_db = v; break;
        case 8: s.lf.freq = v; break;
        case 9: s.lf.q = v; break;
        case 10: s.lmf.gain_db = v; break;
        case 11: s.lmf.freq = v; break;
        case 12: s.lmf.q = v; break;
        case 13: s.hmf.gain_db = v; break;
        case 14: s.hmf.freq = v; break;
        case 15: s.hmf.q = v; break;
        case 16: s.hf.gain_db = v; break;
        case 17: s.hf.freq = v; break;
        case 18: s.hf.q = v; break;
        case 19: s.comp_on = on; break;
        case 20: s.comp_threshold_db = v; break;
        case 21: s.comp_ratio = v; break;
        case 22: s.comp_attack_ms = v; break;
        case 23: s.comp_release_ms = v; break;
        case 24: s.comp_makeup_db = v; break;
        case 25: s.gate_on = on; break;
        case 26: s.gate_threshold_db = v; break;
        case 27: s.gate_range_db = v; break;
        case 28: s.gate_release_ms = v; break;
        default: break;
    }
}

// --- Delay lines ---------------------------------------------------------------------------

DelayLine::DelayLine(int capacity)
{
    int size = 1;
    while (size < capacity) size <<= 1;
    ring_.setSize(2, size);
    ring_.clear();
    mask_ = size - 1;
}

void DelayLine::process(float* l, float* r, int n, int delay) noexcept
{
    float* bl = ring_.getWritePointer(0);
    float* br = ring_.getWritePointer(1);
    int w = write_;
    for (int i = 0; i < n; ++i) {
        bl[w] = l[i];
        br[w] = r[i];
        const int rd = (w - delay) & mask_;
        l[i] = bl[rd];
        r[i] = br[rd];
        w = (w + 1) & mask_;
    }
    write_ = w;
}

void DelayLine::send(const float* l, const float* r, int n, float from_l, float to_l, float from_r, float to_r,
                     float* dl, float* dr, int delay) noexcept
{
    float* bl = ring_.getWritePointer(0);
    float* br = ring_.getWritePointer(1);
    const float step_l = (to_l - from_l) / static_cast<float>(std::max(1, n));
    const float step_r = (to_r - from_r) / static_cast<float>(std::max(1, n));
    int w = write_;
    float gl = from_l, gr = from_r;
    for (int i = 0; i < n; ++i) {
        bl[w] = l[i] * gl;
        br[w] = r[i] * gr;
        gl += step_l;
        gr += step_r;
        const int rd = (w - delay) & mask_;
        dl[i] += bl[rd];
        dr[i] += br[rd];
        w = (w + 1) & mask_;
    }
    write_ = w;
}

void DelayLine::silence(int n) noexcept
{
    float* bl = ring_.getWritePointer(0);
    float* br = ring_.getWritePointer(1);
    for (int i = 0; i < n; ++i) {
        bl[write_] = br[write_] = 0.0f;
        write_ = (write_ + 1) & mask_;
    }
}

void DelayLine::clear() noexcept { ring_.clear(); }

// --- Gestures ------------------------------------------------------------------------------

GestureQueue::GestureQueue()
{
    for (std::size_t i = 0; i < kSize; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
}

bool GestureQueue::push(const GestureEvent& e) noexcept
{
    std::size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
        Cell& cell = cells_[pos & (kSize - 1)];
        const std::size_t seq = cell.seq.load(std::memory_order_acquire);
        const auto dif = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
        if (dif == 0) {
            if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                cell.data = e;
                cell.seq.store(pos + 1, std::memory_order_release);
                return true;
            }
        } else if (dif < 0) {
            return false;  // full: the gesture is dropped, never blocks
        } else {
            pos = head_.load(std::memory_order_relaxed);
        }
    }
}

bool GestureQueue::pop(GestureEvent& e) noexcept
{
    Cell& cell = cells_[tail_ & (kSize - 1)];
    const std::size_t seq = cell.seq.load(std::memory_order_acquire);
    if (static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(tail_ + 1) < 0) return false;
    e = cell.data;
    cell.seq.store(tail_ + kSize, std::memory_order_release);
    ++tail_;
    return true;
}

ParameterWatch::ParameterWatch(juce::AudioProcessor& processor, std::shared_ptr<GestureQueue> queue, int id)
    : processor_(processor), queue_(std::move(queue)), id_(id)
{
    for (auto* p : processor_.getParameters()) p->addListener(this);
}

ParameterWatch::~ParameterWatch()
{
    for (auto* p : processor_.getParameters()) p->removeListener(this);
}

void ParameterWatch::parameterValueChanged(int index, float value)
{
    queue_->push({id_, index, 0, value, queue_->heard.load(std::memory_order_relaxed)});
}

void ParameterWatch::parameterGestureChanged(int index, bool starting)
{
    queue_->push({id_, index, starting ? 1 : 2, 0.0f, queue_->heard.load(std::memory_order_relaxed)});
}

}  // namespace vcc::desktop
