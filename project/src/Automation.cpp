#include "vcc/project/Automation.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace vcc::project {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// Reads or writes one numeric ChannelStrip field by param id.
struct StripField {
    ParamInfo info;
    std::function<double&(ChannelStrip&)> ref;
    std::function<bool&(ChannelStrip&)> flag;  ///< switches
};

ParamInfo P(const std::string& id, const std::string& name, const std::string& unit, double mn, double mx,
            double def, bool toggle = false, bool log = false)
{
    ParamInfo p;
    p.id = id;
    p.name = name;
    p.unit = unit;
    p.min = mn;
    p.max = mx;
    p.default_value = def;
    p.toggle = toggle;
    p.log_scale = log;
    return p;
}

const std::vector<StripField>& strip_fields()
{
    static const std::vector<StripField> fields = [] {
        std::vector<StripField> f;
        auto num = [&](const std::string& id, const std::string& name, const std::string& unit, double mn, double mx,
                       double def, std::function<double&(ChannelStrip&)> r, bool log = false) {
            f.push_back({P("strip." + id, name, unit, mn, mx, def, false, log), std::move(r), {}});
        };
        auto sw = [&](const std::string& id, const std::string& name, bool def,
                      std::function<bool&(ChannelStrip&)> r) {
            f.push_back({P("strip." + id, name, "", 0, 1, def ? 1 : 0, true), {}, std::move(r)});
        };
        const ChannelStrip d;
        num("trim_db", "Trim", "dB", -20, 20, d.trim_db, [](ChannelStrip& s) -> double& { return s.trim_db; });
        sw("phase_invert", "Phase", d.phase_invert, [](ChannelStrip& s) -> bool& { return s.phase_invert; });
        sw("hpf_on", "HPF In", d.hpf_on, [](ChannelStrip& s) -> bool& { return s.hpf_on; });
        num("hpf_hz", "HPF Freq", "Hz", 20, 400, d.hpf_hz, [](ChannelStrip& s) -> double& { return s.hpf_hz; }, true);
        sw("lpf_on", "LPF In", d.lpf_on, [](ChannelStrip& s) -> bool& { return s.lpf_on; });
        num("lpf_hz", "LPF Freq", "Hz", 3000, 20000, d.lpf_hz, [](ChannelStrip& s) -> double& { return s.lpf_hz; },
            true);
        sw("eq_on", "EQ In", d.eq_on, [](ChannelStrip& s) -> bool& { return s.eq_on; });
        struct Band {
            const char* id;
            const char* name;
            EqBand ChannelStrip::*band;
            double fmin, fmax;
        };
        for (const Band b : {Band{"lf", "LF", &ChannelStrip::lf, 30, 450}, Band{"lmf", "LMF", &ChannelStrip::lmf, 100, 2000},
                             Band{"hmf", "HMF", &ChannelStrip::hmf, 600, 7000},
                             Band{"hf", "HF", &ChannelStrip::hf, 1500, 16000}}) {
            const EqBand& db = d.*(b.band);
            auto band = b.band;
            num(std::string(b.id) + ".gain_db", std::string(b.name) + " Gain", "dB", -18, 18, db.gain_db,
                [band](ChannelStrip& s) -> double& { return (s.*band).gain_db; });
            num(std::string(b.id) + ".freq", std::string(b.name) + " Freq", "Hz", b.fmin, b.fmax, db.freq,
                [band](ChannelStrip& s) -> double& { return (s.*band).freq; }, true);
            num(std::string(b.id) + ".q", std::string(b.name) + " Q", "", 0.3, 6, db.q,
                [band](ChannelStrip& s) -> double& { return (s.*band).q; }, true);
        }
        sw("comp_on", "Comp In", d.comp_on, [](ChannelStrip& s) -> bool& { return s.comp_on; });
        num("comp_threshold_db", "Comp Threshold", "dB", -60, 0, d.comp_threshold_db,
            [](ChannelStrip& s) -> double& { return s.comp_threshold_db; });
        num("comp_ratio", "Comp Ratio", ":1", 1, 20, d.comp_ratio,
            [](ChannelStrip& s) -> double& { return s.comp_ratio; }, true);
        num("comp_attack_ms", "Comp Attack", "ms", 0.1, 100, d.comp_attack_ms,
            [](ChannelStrip& s) -> double& { return s.comp_attack_ms; }, true);
        num("comp_release_ms", "Comp Release", "ms", 10, 2000, d.comp_release_ms,
            [](ChannelStrip& s) -> double& { return s.comp_release_ms; }, true);
        num("comp_makeup_db", "Comp Gain", "dB", 0, 24, d.comp_makeup_db,
            [](ChannelStrip& s) -> double& { return s.comp_makeup_db; });
        sw("gate_on", "Gate In", d.gate_on, [](ChannelStrip& s) -> bool& { return s.gate_on; });
        num("gate_threshold_db", "Gate Threshold", "dB", -80, 0, d.gate_threshold_db,
            [](ChannelStrip& s) -> double& { return s.gate_threshold_db; });
        num("gate_range_db", "Gate Range", "dB", 0, 80, d.gate_range_db,
            [](ChannelStrip& s) -> double& { return s.gate_range_db; });
        num("gate_release_ms", "Gate Release", "ms", 10, 2000, d.gate_release_ms,
            [](ChannelStrip& s) -> double& { return s.gate_release_ms; }, true);
        return f;
    }();
    return fields;
}

const StripField* strip_field(const std::string& id)
{
    for (const auto& f : strip_fields())
        if (f.info.id == id) return &f;
    return nullptr;
}

/// Built-in insert params, as the engine documents them in ChannelDsp.h.
const std::map<std::string, std::vector<ParamInfo>>& insert_params()
{
    static const std::map<std::string, std::vector<ParamInfo>> table = {
        {"compressor",
         {P("threshold_db", "Threshold", "dB", -60, 0, -18), P("ratio", "Ratio", ":1", 1, 20, 3, false, true),
          P("attack_ms", "Attack", "ms", 0.1, 100, 10, false, true),
          P("release_ms", "Release", "ms", 10, 2000, 120, false, true), P("makeup_db", "Gain", "dB", 0, 24, 0)}},
        {"gate",
         {P("threshold_db", "Threshold", "dB", -80, 0, -50), P("range_db", "Range", "dB", 0, 80, 40),
          P("release_ms", "Release", "ms", 10, 2000, 150, false, true)}},
        {"reverb",
         {P("room", "Room", "", 0, 1, 0.82), P("damping", "Damping", "", 0, 1, 0.4), P("width", "Width", "", 0, 1, 1),
          P("mix", "Mix", "", 0, 1, 1)}},
        {"delay",
         {P("time_ms", "Time", "ms", 1, 2000, 375, false, true), P("feedback", "Feedback", "", 0, 0.95, 0.35),
          P("mix", "Mix", "", 0, 1, 1)}},
        {"limiter",
         {P("ceiling_db", "Ceiling", "dB", -24, 0, -0.3), P("release_ms", "Release", "ms", 1, 1000, 80, false, true)}},
        {"saturation",
         {P("drive_db", "Drive", "dB", 0, 36, 6), P("mix", "Mix", "", 0, 1, 1), P("output_db", "Output", "dB", -24, 12, 0)}},
        {"eq",
         {P("lf_gain", "LF Gain", "dB", -18, 18, 0), P("lf_freq", "LF Freq", "Hz", 30, 450, 100, false, true),
          P("lmf_gain", "LMF Gain", "dB", -18, 18, 0), P("lmf_freq", "LMF Freq", "Hz", 100, 2000, 400, false, true),
          P("hmf_gain", "HMF Gain", "dB", -18, 18, 0), P("hmf_freq", "HMF Freq", "Hz", 600, 7000, 2500, false, true),
          P("hf_gain", "HF Gain", "dB", -18, 18, 0), P("hf_freq", "HF Freq", "Hz", 1500, 16000, 8000, false, true)}},
    };
    return table;
}

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const auto pos = s.find(sep, start);
        out.push_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return out;
}

double clamp_to(const ParamInfo& p, double v)
{
    if (!std::isfinite(v)) v = p.default_value;
    v = std::clamp(v, p.min, p.max);
    if (p.toggle) v = v >= 0.5 ? 1.0 : 0.0;
    return v;
}

std::vector<ParamInfo> common_params(const std::vector<Send>& sends, const std::vector<InsertSlot>& inserts,
                                     const ProjectData& data)
{
    std::vector<ParamInfo> out = strip_params();
    for (const auto& s : sends) {
        const Bus* b = data.find_bus(s.bus_id);
        ParamInfo p = P("send:" + s.bus_id, "Send " + (b ? b->name : s.bus_id), "dB", -96, 12, s.level_db);
        out.push_back(p);
    }
    for (std::size_t i = 0; i < inserts.size(); ++i) {
        auto it = insert_params().find(inserts[i].kind);
        if (it == insert_params().end()) continue;
        for (ParamInfo p : it->second) {
            p.name = inserts[i].kind + " " + std::to_string(i + 1) + " " + p.name;
            p.id = "insert:" + std::to_string(i) + ":" + p.id;
            out.push_back(p);
        }
    }
    return out;
}

template <typename Owner>
std::optional<double> static_value_impl(const Owner& o, const std::string& param)
{
    if (param == "volume") return o.gain_db;
    if (param == "pan") return o.pan;
    if (param == "mute") return o.mute ? 1.0 : 0.0;
    if (param.rfind("send:", 0) == 0) {
        const std::string bus = param.substr(5);
        for (const auto& s : o.sends)
            if (s.bus_id == bus) return s.level_db;
        return std::nullopt;
    }
    if (param.rfind("strip.", 0) == 0) {
        const StripField* f = strip_field(param);
        if (!f) return std::nullopt;
        ChannelStrip copy = o.strip;
        return f->ref ? f->ref(copy) : (f->flag(copy) ? 1.0 : 0.0);
    }
    if (param.rfind("insert:", 0) == 0) {
        const auto parts = split(param, ':');
        if (parts.size() != 3) return std::nullopt;
        const auto slot = static_cast<std::size_t>(std::stoul(parts[1]));
        if (slot >= o.inserts.size()) return std::nullopt;
        auto it = o.inserts[slot].params.find(parts[2]);
        if (it != o.inserts[slot].params.end()) return it->second;
        auto info = param_info(param);
        return info ? std::optional<double>(info->default_value) : std::nullopt;
    }
    return std::nullopt;  // plug-in params live in the plug-in's state
}

template <typename Owner>
bool set_static_value_impl(Owner& o, const std::string& param, double value)
{
    const auto info = param_info(param);
    if (!info) return false;
    value = clamp_to(*info, value);
    if (param == "volume") { o.gain_db = value; return true; }
    if (param == "pan") { o.pan = value; return true; }
    if (param == "mute") { o.mute = value >= 0.5; return true; }
    if (param.rfind("send:", 0) == 0) {
        for (auto& s : o.sends)
            if (s.bus_id == param.substr(5)) { s.level_db = value; return true; }
        return false;
    }
    if (param.rfind("strip.", 0) == 0) {
        const StripField* f = strip_field(param);
        if (!f) return false;
        if (f->ref) f->ref(o.strip) = value;
        else f->flag(o.strip) = value >= 0.5;
        return true;
    }
    if (param.rfind("insert:", 0) == 0) {
        const auto parts = split(param, ':');
        const auto slot = static_cast<std::size_t>(std::stoul(parts[1]));
        if (slot >= o.inserts.size()) return false;
        o.inserts[slot].params[parts[2]] = value;
        return true;
    }
    return false;
}

}  // namespace

const std::vector<ParamInfo>& strip_params()
{
    static const std::vector<ParamInfo> params = [] {
        std::vector<ParamInfo> p = {P("volume", "Volume", "dB", -96, 12, 0), P("pan", "Pan", "", -1, 1, 0),
                                    P("mute", "Mute", "", 0, 1, 0, true)};
        for (const auto& f : strip_fields()) p.push_back(f.info);
        return p;
    }();
    return params;
}

std::vector<ParamInfo> automatable_params(const Track& track, const ProjectData& data)
{
    return common_params(track.sends, track.inserts, data);
}

std::vector<ParamInfo> automatable_params(const Bus& bus, const ProjectData& data)
{
    return common_params(bus.sends, bus.inserts, data);
}

std::optional<ParamInfo> param_info(const std::string& param)
{
    for (const auto& p : strip_params())
        if (p.id == param) return p;
    if (param.rfind("send:", 0) == 0 && param.size() > 5)
        return P(param, "Send", "dB", -96, 12, -12);
    const auto parts = split(param, ':');
    if (parts[0] == "insert" && parts.size() == 3) {
        try {
            (void)std::stoul(parts[1]);
        } catch (...) {
            return std::nullopt;
        }
        for (const auto& [kind, list] : insert_params())
            for (ParamInfo p : list)
                if (p.id == parts[2]) {
                    p.id = param;
                    return p;
                }
        return P(param, parts[2], "", -1e9, 1e9, 0);  // a param this build does not describe
    }
    if ((parts[0] == "plugin" && parts.size() == 2) || (parts[0] == "insert_plugin" && parts.size() == 3)) {
        try {
            for (std::size_t i = 1; i < parts.size(); ++i) (void)std::stoul(parts[i]);
        } catch (...) {
            return std::nullopt;
        }
        return P(param, "Plug-in parameter", "", 0, 1, 0);
    }
    return std::nullopt;
}

std::optional<double> value_at(const AutomationLane& lane, Ticks tick)
{
    const auto& pts = lane.points;
    if (pts.empty()) return std::nullopt;
    if (tick <= pts.front().tick) return pts.front().value;
    if (tick >= pts.back().tick) return pts.back().value;
    auto right = std::upper_bound(pts.begin(), pts.end(), tick,
                                  [](Ticks t, const AutomationPoint& p) { return t < p.tick; });
    const AutomationPoint& b = *right;
    const AutomationPoint& a = *(right - 1);
    if (a.shape == "step" || b.tick == a.tick) return a.value;
    double x = static_cast<double>(tick - a.tick) / static_cast<double>(b.tick - a.tick);
    if (a.shape == "smooth") x = 0.5 - 0.5 * std::cos(kPi * x);
    return a.value + (b.value - a.value) * x;
}

std::optional<double> static_value(const Track& track, const std::string& param)
{
    return static_value_impl(track, param);
}
std::optional<double> static_value(const Bus& bus, const std::string& param) { return static_value_impl(bus, param); }
bool set_static_value(Track& track, const std::string& param, double value)
{
    return set_static_value_impl(track, param, value);
}
bool set_static_value(Bus& bus, const std::string& param, double value)
{
    return set_static_value_impl(bus, param, value);
}

void normalise(AutomationLane& lane)
{
    const auto info = param_info(lane.param);
    std::stable_sort(lane.points.begin(), lane.points.end(),
                     [](const AutomationPoint& a, const AutomationPoint& b) { return a.tick < b.tick; });
    std::vector<AutomationPoint> out;
    for (auto p : lane.points) {
        p.tick = std::max<Ticks>(0, p.tick);
        if (info) p.value = clamp_to(*info, p.value);
        if (p.shape != "linear" && p.shape != "step" && p.shape != "smooth") p.shape = "linear";
        if (info && info->toggle) p.shape = "step";
        if (!out.empty() && out.back().tick == p.tick) out.back() = p;
        else out.push_back(p);
    }
    lane.points = std::move(out);
}

void thin(std::vector<AutomationPoint>& points, double tolerance)
{
    if (points.size() < 3) return;
    std::vector<bool> keep(points.size(), false);
    keep.front() = keep.back() = true;
    // Douglas-Peucker over (tick, value), measuring the vertical error.
    std::vector<std::pair<std::size_t, std::size_t>> stack = {{0, points.size() - 1}};
    while (!stack.empty()) {
        auto [a, b] = stack.back();
        stack.pop_back();
        if (b <= a + 1) continue;
        double worst = -1.0;
        std::size_t at = a;
        for (std::size_t i = a + 1; i < b; ++i) {
            const double x = static_cast<double>(points[i].tick - points[a].tick) /
                             static_cast<double>(std::max<Ticks>(1, points[b].tick - points[a].tick));
            const double line = points[a].value + (points[b].value - points[a].value) * x;
            const double err = std::abs(points[i].value - line) + (points[i].shape != "linear" ? 1e9 : 0.0);
            if (err > worst) {
                worst = err;
                at = i;
            }
        }
        if (worst > tolerance) {
            keep[at] = true;
            stack.push_back({a, at});
            stack.push_back({at, b});
        }
    }
    std::vector<AutomationPoint> out;
    for (std::size_t i = 0; i < points.size(); ++i)
        if (keep[i]) out.push_back(points[i]);
    points = std::move(out);
}

void write_pass(AutomationLane& lane, Ticks from, Ticks to, std::vector<AutomationPoint> written)
{
    if (to < from) std::swap(from, to);
    const auto info = param_info(lane.param);
    const auto before = value_at(lane, from);
    const auto after = value_at(lane, to);
    AutomationLane w;
    w.param = lane.param;
    w.points = std::move(written);
    normalise(w);
    if (!info || !info->toggle) {
        const double range = info ? (info->max - info->min) : 1.0;
        const double tol = info && info->unit == "dB" ? 0.25 : range * 0.002;
        thin(w.points, tol);
    }
    std::vector<AutomationPoint> out;
    for (const auto& p : lane.points)
        if (p.tick < from) out.push_back(p);
    if (before && (w.points.empty() || w.points.front().tick > from)) out.push_back({from, *before, "linear"});
    for (const auto& p : w.points)
        if (p.tick >= from && p.tick <= to) out.push_back(p);
    // Return to what the lane held after the pass, one tick later, so the
    // written value does not ramp into the old automation.
    if (after && to > from) out.push_back({to + 1, *after, "linear"});
    for (const auto& p : lane.points)
        if (p.tick > to + 1) out.push_back(p);
    lane.points = std::move(out);
    normalise(lane);
}

const AutomationLane* find_lane(const std::vector<AutomationLane>& lanes, const std::string& param)
{
    for (const auto& l : lanes)
        if (l.param == param) return &l;
    return nullptr;
}

AutomationLane* find_lane(std::vector<AutomationLane>& lanes, const std::string& param)
{
    for (auto& l : lanes)
        if (l.param == param) return &l;
    return nullptr;
}

}  // namespace vcc::project
