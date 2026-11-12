#include "ui/AutomationLaneView.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "ui/Theme.h"

namespace vcc::desktop {

namespace automation_ui {

namespace {

constexpr int kLaw = 4;  ///< Fader law: position = (gain / max gain)^(1/4).

bool fader_law(const project::ParamInfo& p) { return p.unit == "dB" && p.min <= -60.0 && p.max > 0.0; }
bool log_map(const project::ParamInfo& p) { return p.log_scale && p.min > 0.0 && p.max > p.min; }
bool is_pan(const project::ParamInfo& p) { return p.id == "pan"; }
bool unit_range(const project::ParamInfo& p)
{
    return p.unit.empty() && !p.toggle && std::abs(p.min) < 1e-9 && p.max <= 1.0 + 1e-9 && p.max > 0.5;
}

double law(double db, double max_db) { return std::pow(std::pow(10.0, (db - max_db) / 20.0), 1.0 / kLaw); }

/// A finite range to draw, even for params this build does not describe.
std::pair<double, double> span(const project::ParamInfo& p)
{
    if (p.max - p.min > 1e6 || p.max <= p.min) return {-1.0, 1.0};
    return {p.min, p.max};
}

juce::String trim_number(double v, int decimals)
{
    juce::String s(v, decimals);
    if (s.containsChar('.')) s = s.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
    return s == "-0" ? juce::String("0") : s;
}

}  // namespace

double to_norm(const project::ParamInfo& p, double v)
{
    if (p.toggle) return v >= 0.5 ? 1.0 : 0.0;
    const auto [lo, hi] = span(p);
    v = std::clamp(v, lo, hi);
    if (fader_law(p)) {
        if (v <= lo) return 0.0;
        const double f0 = law(lo, hi);
        return std::clamp((law(v, hi) - f0) / (1.0 - f0), 0.0, 1.0);
    }
    if (log_map(p)) return std::log(v / lo) / std::log(hi / lo);
    return (v - lo) / (hi - lo);
}

double from_norm(const project::ParamInfo& p, double n)
{
    n = std::clamp(n, 0.0, 1.0);
    if (p.toggle) return n >= 0.5 ? 1.0 : 0.0;
    const auto [lo, hi] = span(p);
    if (fader_law(p)) {
        if (n <= 0.0) return lo;
        const double f0 = law(lo, hi);
        const double f = f0 + n * (1.0 - f0);
        return std::clamp(hi + 20.0 * std::log10(std::pow(f, kLaw)), lo, hi);
    }
    if (log_map(p)) return lo * std::pow(hi / lo, n);
    return lo + n * (hi - lo);
}

juce::String format_value(const project::ParamInfo& p, double v)
{
    if (p.toggle) return v >= 0.5 ? "On" : "Off";
    if (is_pan(p)) {
        const int pct = juce::roundToInt(std::abs(v) * 100.0);
        if (pct == 0) return "C";
        return (v < 0 ? "L" : "R") + juce::String(pct);
    }
    if (p.unit == "dB") {
        if (fader_law(p) && v <= p.min + 1e-9) return "-inf dB";
        const double r = std::round(v * 10.0) / 10.0;
        return (r > 0.0 ? "+" : "") + juce::String(std::abs(r) < 0.05 ? 0.0 : r, 1) + " dB";
    }
    if (p.unit == "Hz") {
        if (v >= 1000.0) return juce::String(v / 1000.0, v >= 10000.0 ? 1 : 2) + " kHz";
        return juce::String(juce::roundToInt(v)) + " Hz";
    }
    if (p.unit == "ms") {
        if (v >= 1000.0) return juce::String(v / 1000.0, 2) + " s";
        return (v < 10.0 ? juce::String(v, 1) : juce::String(juce::roundToInt(v))) + " ms";
    }
    if (p.unit == ":1") return juce::String(v, 1) + ":1";
    if (unit_range(p)) return juce::String(juce::roundToInt(v * 100.0)) + "%";
    if (p.unit.empty()) return juce::String(v, 2);
    return juce::String(v, 2) + " " + juce::String(p.unit);
}

std::optional<double> parse_value(const project::ParamInfo& p, const juce::String& text)
{
    juce::String t = text.trim().toLowerCase();
    if (t.isEmpty()) return std::nullopt;
    auto clamp = [&](double v) {
        const auto [lo, hi] = span(p);
        v = std::clamp(v, lo, hi);
        if (p.toggle) v = v >= 0.5 ? 1.0 : 0.0;
        return v;
    };
    if (p.toggle) {
        if (t == "on" || t == "yes" || t == "true") return 1.0;
        if (t == "off" || t == "no" || t == "false") return 0.0;
    }
    if (t.contains("inf")) return clamp(t.startsWithChar('+') ? p.max : p.min);
    if (is_pan(p)) {
        if (t == "c" || t == "centre" || t == "center") return 0.0;
        if (t.startsWithChar('l')) return clamp(-t.substring(1).trim().getDoubleValue() / 100.0);
        if (t.startsWithChar('r')) return clamp(t.substring(1).trim().getDoubleValue() / 100.0);
    }
    if (!t.containsAnyOf("0123456789")) return std::nullopt;
    double v = t.retainCharacters("0123456789.-+").getDoubleValue();
    if (p.unit == "Hz" && t.containsChar('k')) v *= 1000.0;
    if (p.unit == "ms" && t.endsWith("s") && !t.endsWith("ms")) v *= 1000.0;
    if (unit_range(p) && (t.containsChar('%') || v > 1.0)) v /= 100.0;
    else if (is_pan(p) && std::abs(v) > 1.0) v /= 100.0;
    return clamp(v);
}

std::vector<GridLine> grid_lines(const project::ParamInfo& p, int height_px)
{
    std::vector<double> values;
    const auto [lo, hi] = span(p);
    if (p.toggle) {
        values = {0.0, 1.0};
    } else if (fader_law(p)) {
        for (double v : {12.0, 6.0, 0.0, -6.0, -12.0, -18.0, -24.0, -36.0, -48.0, -60.0})
            if (v <= hi && v > lo) values.push_back(v);
        values.push_back(lo);
    } else if (is_pan(p)) {
        values = {-1.0, -0.5, 0.0, 0.5, 1.0};
    } else if (log_map(p)) {
        for (double decade = std::pow(10.0, std::floor(std::log10(lo))); decade <= hi; decade *= 10.0)
            for (double m : {1.0, 2.0, 5.0})
                if (const double v = m * decade; v >= lo - 1e-9 && v <= hi + 1e-9) values.push_back(v);
    } else {
        const double target = std::max(2, height_px / 22);
        const double raw = (hi - lo) / target;
        const double mag = std::pow(10.0, std::floor(std::log10(raw)));
        double step = mag;
        for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
            if (m * mag >= raw) {
                step = m * mag;
                break;
            }
        for (double v = std::ceil(lo / step - 1e-9) * step; v <= hi + 1e-9; v += step) values.push_back(v);
    }
    // Keep lines far enough apart for their labels (top first).
    std::sort(values.begin(), values.end(), [&](double a, double b) { return to_norm(p, a) > to_norm(p, b); });
    std::vector<GridLine> out;
    double last = 1e9;
    const double min_gap = 11.0 / std::max(1, height_px - 14);
    for (double v : values) {
        const double n = to_norm(p, v);
        if (last - n < min_gap && !(p.toggle)) continue;
        last = n;
        juce::String label = format_value(p, v);
        if (p.unit == "dB") label = label.upToFirstOccurrenceOf(" ", false, false);
        if (p.unit == "dB" && label.endsWith(".0")) label = label.dropLastCharacters(2);
        if (p.unit == "Hz") label = label.replace(" kHz", "k").replace(" Hz", "");
        if (p.unit == "ms") label = trim_number(v, 1);
        if (unit_range(p) || p.unit == ":1") label = label.replace(":1", "");
        out.push_back({v, label});
    }
    return out;
}

juce::Colour mode_colour(const std::string& mode)
{
    if (mode == "read") return juce::Colour(0xff4fbf73);
    if (mode == "touch") return juce::Colour(0xffe8c33d);
    if (mode == "latch") return juce::Colour(0xffe8903d);
    if (mode == "write") return juce::Colour(0xffe8574a);
    return juce::Colour(0xff7d8692);
}

namespace {

std::string section_of(const std::string& id)
{
    auto starts = [&](const char* s) { return id.rfind(s, 0) == 0; };
    if (id == "volume" || id == "pan" || id == "mute" || id == "strip.trim_db" || id == "strip.phase_invert") return {};
    if (starts("send:")) return "Sends";
    if (starts("strip.hpf") || starts("strip.lpf")) return "Input / Filters";
    if (starts("strip.eq") || starts("strip.lf.") || starts("strip.lmf.") || starts("strip.hmf.") || starts("strip.hf."))
        return "EQ";
    if (starts("strip.comp") || starts("strip.gate")) return "Dynamics";
    if (starts("insert:")) return "Inserts";
    return "Other";
}

void add_plugin_params(std::vector<ParamChoice>& out, juce::AudioPluginInstance* plugin, const std::string& prefix,
                       const std::string& section)
{
    if (!plugin) return;
    const auto& params = plugin->getParameters();
    for (int i = 0; i < params.size() && i < 512; ++i) {
        juce::String name = params[i]->getName(48).trim();
        if (name.isEmpty()) name = "Parameter " + juce::String(i + 1);
        out.push_back({prefix + std::to_string(i), name.toStdString(), section});
    }
}

}  // namespace

std::vector<ParamChoice> param_choices(AppContext& context, const std::string& owner_id)
{
    std::vector<ParamChoice> out;
    const auto& d = context.session.data();
    std::vector<project::ParamInfo> params;
    const std::vector<project::InsertSlot>* inserts = nullptr;
    if (const auto* t = d.find_track(owner_id)) {
        params = project::automatable_params(*t, d);
        inserts = &t->inserts;
    } else if (const auto* b = d.find_bus(owner_id)) {
        params = project::automatable_params(*b, d);
        inserts = &b->inserts;
    } else {
        return out;
    }
    for (const auto& p : params) out.push_back({p.id, p.name, section_of(p.id)});
    if (d.find_track(owner_id)) {
        if (auto* plugin = context.engine.plugin_for(owner_id))
            add_plugin_params(out, plugin, "plugin:", "Instrument: " + plugin->getName().toStdString());
    }
    for (std::size_t s = 0; inserts && s < inserts->size(); ++s) {
        if ((*inserts)[s].kind != "plugin") continue;
        if (auto* plugin = context.engine.insert_plugin(owner_id, static_cast<int>(s)))
            add_plugin_params(out, plugin, "insert_plugin:" + std::to_string(s) + ":",
                              "Insert " + std::to_string(s + 1) + ": " + plugin->getName().toStdString());
    }
    return out;
}

void show_param_menu(AppContext& context, const std::string& owner_id, juce::Component* target,
                     const std::function<void(const std::string&, const std::string&)>& chosen)
{
    const auto choices = param_choices(context, owner_id);
    std::set<std::string> shown;
    if (const auto* lanes = context.session.lanes_of(owner_id))
        for (const auto& l : *lanes)
            if (l.shown) shown.insert(l.param);
    juce::PopupMenu menu;
    std::vector<std::string> order;
    std::map<std::string, juce::PopupMenu> subs;
    for (const auto& c : choices) {
        auto action = [chosen, id = c.id, name = c.name] { chosen(id, name); };
        const bool ticked = shown.count(c.id) > 0;
        if (c.section.empty()) {
            menu.addItem(juce::String(c.name), true, ticked, action);
            continue;
        }
        if (!subs.count(c.section)) order.push_back(c.section);
        auto& sub = subs[c.section];
        // Long plug-in lists break into columns.
        if (sub.getNumItems() > 0 && sub.getNumItems() % 40 == 0) sub.addColumnBreak();
        sub.addItem(juce::String(c.name), true, ticked, action);
    }
    for (const char* fixed : {"Sends", "Input / Filters", "EQ", "Dynamics", "Inserts"}) {
        if (subs.count(fixed)) menu.addSubMenu(fixed, subs[fixed]);
        else if (std::string(fixed) == "Sends") menu.addItem("Sends (none)", false, false, [] {});
    }
    bool separated = false;
    for (const auto& s : order) {
        if (s == "Sends" || s == "Input / Filters" || s == "EQ" || s == "Dynamics" || s == "Inserts") continue;
        if (!separated) menu.addSeparator();
        separated = true;
        menu.addSubMenu(juce::String(s), subs[s]);
    }
    if (!separated && context.session.data().find_track(owner_id)) {
        menu.addSeparator();
        menu.addItem("Plug-in parameters appear when a plug-in is loaded", false, false, [] {});
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target));
}

}  // namespace automation_ui

namespace {

using namespace automation_ui;

constexpr int kPad = 7;            ///< Space above the top value and below the bottom one.
constexpr float kHandle = 3.6f;    ///< Breakpoint handle radius.
constexpr float kGrab = 6.0f;      ///< How close a click must be to a handle.
constexpr int kResizeZone = 4;     ///< Bottom edge that resizes the row.
const juce::Colour kBodyColour{0xff1a1d22};

/// Automation copied with Cmd/Ctrl+C: ticks relative to the first point and
/// normalised values, so it pastes onto any lane.
struct Clipboard {
    std::vector<std::pair<Ticks, double>> points;
    std::vector<std::string> shapes;
    Ticks length = 0;
};
Clipboard& clipboard()
{
    static Clipboard c;
    return c;
}

void draw_icon(juce::Graphics& g, AutomationLaneView::Part part, juce::Rectangle<float> r, juce::Colour c, bool on)
{
    const auto centre = r.getCentre();
    g.setColour(c);
    switch (part) {
        case AutomationLaneView::Part::Power: {
            juce::Path arc;
            const float rad = 4.2f;
            arc.addCentredArc(centre.x, centre.y + 0.5f, rad, rad, 0.0f, 0.75f, juce::MathConstants<float>::twoPi - 0.75f,
                              true);
            g.strokePath(arc, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.drawLine(centre.x, centre.y - 5.0f, centre.x, centre.y - 0.5f, 1.4f);
            (void)on;
            break;
        }
        case AutomationLaneView::Part::Hide: {
            juce::Path eye;
            eye.startNewSubPath(centre.x - 5.5f, centre.y);
            eye.quadraticTo(centre.x, centre.y - 5.0f, centre.x + 5.5f, centre.y);
            eye.quadraticTo(centre.x, centre.y + 5.0f, centre.x - 5.5f, centre.y);
            g.strokePath(eye, juce::PathStrokeType(1.2f));
            g.fillEllipse(centre.x - 1.6f, centre.y - 1.6f, 3.2f, 3.2f);
            g.drawLine(centre.x - 5.0f, centre.y + 4.5f, centre.x + 5.0f, centre.y - 4.5f, 1.2f);
            break;
        }
        case AutomationLaneView::Part::Remove:
            g.drawLine(centre.x - 3.5f, centre.y - 3.5f, centre.x + 3.5f, centre.y + 3.5f, 1.4f);
            g.drawLine(centre.x - 3.5f, centre.y + 3.5f, centre.x + 3.5f, centre.y - 3.5f, 1.4f);
            break;
        case AutomationLaneView::Part::Add:
            g.drawLine(centre.x - 4.0f, centre.y, centre.x + 4.0f, centre.y, 1.5f);
            g.drawLine(centre.x, centre.y - 4.0f, centre.x, centre.y + 4.0f, 1.5f);
            break;
        case AutomationLaneView::Part::Name:
        case AutomationLaneView::Part::Readout:
        case AutomationLaneView::Part::Mode: break;
    }
}

juce::String mode_text(const std::string& mode) { return juce::String(mode).toUpperCase(); }

}  // namespace

// --- AutomationLaneView ---------------------------------------------------------------------------

AutomationLaneView::AutomationLaneView(AutomationTimeline& timeline) : tl_(timeline)
{
    setOpaque(true);
    setWantsKeyboardFocus(true);
    setRepaintsOnMouseActivity(false);
}

AutomationLaneView::~AutomationLaneView() = default;

void AutomationLaneView::set_data(Data data)
{
    const bool same_lane = data_.lane && data.lane && data_.lane->id == data.lane->id && data_.owner_id == data.owner_id;
    data_ = std::move(data);
    info_ = data_.lane ? project::param_info(data_.lane->param) : std::nullopt;
    if (info_ && data_.lane && (info_->name == "Plug-in parameter" || info_->name.empty())) info_->name = data_.lane->name;
    if (drag_ != Drag::None && same_lane) {
        // A document change in the middle of a gesture (another view): drop the gesture.
        drag_ = Drag::None;
        bubble_.clear();
    }
    work_ = data_.lane ? *data_.lane : project::AutomationLane{};
    if (!same_lane) selection_.clear();
    std::set<Ticks> kept;
    for (const auto& p : work_.points)
        if (selection_.count(p.tick)) kept.insert(p.tick);
    selection_ = std::move(kept);
    hover_point_ = -1;
    setTitle(data_.lane ? juce::String(data_.lane->name) + " automation" : juce::String("Add automation"));
    repaint();
}

void AutomationLaneView::playhead_moved()
{
    const auto text = readout();
    if (text == last_readout_) return;
    last_readout_ = text;
    repaint(part_bounds(Part::Readout));
}

juce::String AutomationLaneView::readout() const
{
    if (!data_.lane || !info_) return {};
    std::optional<double> v = tl_.context().engine.automated_value(data_.owner_id, data_.lane->param);
    if (!v || drag_ != Drag::None) v = project::value_at(work_, tl_.playhead_tick());
    return v ? format_value(*info_, *v) : juce::String();
}

void AutomationLaneView::resized() {}

// --- Geometry --------------------------------------------------------------------------------------

juce::Rectangle<int> AutomationLaneView::body() const
{
    return getLocalBounds().withTrimmedLeft(tl_.header_width()).withTrimmedBottom(1);
}

float AutomationLaneView::y_of_norm(double n) const
{
    const float top = static_cast<float>(kPad), bottom = static_cast<float>(getHeight() - 1 - kPad);
    return bottom - static_cast<float>(n) * (bottom - top);
}

double AutomationLaneView::norm_of_y(float y) const
{
    const float top = static_cast<float>(kPad), bottom = static_cast<float>(getHeight() - 1 - kPad);
    return std::clamp(static_cast<double>((bottom - y) / std::max(1.0f, bottom - top)), 0.0, 1.0);
}

float AutomationLaneView::y_of_value(double v) const { return info_ ? y_of_norm(to_norm(*info_, v)) : 0.0f; }
double AutomationLaneView::value_of_y(float y) const { return info_ ? from_norm(*info_, norm_of_y(y)) : 0.0; }
double AutomationLaneView::x_of(Ticks t) const { return tl_.x_of_tick(static_cast<double>(t)); }
Ticks AutomationLaneView::tick_of(float x) const
{
    return std::max<Ticks>(0, static_cast<Ticks>(std::llround(tl_.tick_at_x(static_cast<double>(x)))));
}

juce::Point<float> AutomationLaneView::position_of(Ticks tick, double value) const
{
    return {static_cast<float>(x_of(tick)), y_of_value(value)};
}

std::optional<double> AutomationLaneView::curve_value(Ticks t) const { return project::value_at(work_, t); }

int AutomationLaneView::point_at(juce::Point<float> p) const
{
    int best = -1;
    float best_d = kGrab * kGrab;
    for (std::size_t i = 0; i < points().size(); ++i) {
        const auto q = position_of(points()[i].tick, points()[i].value);
        const float d = q.getDistanceSquaredFrom(p);
        if (d <= best_d) {
            best_d = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

juce::Rectangle<int> AutomationLaneView::part_bounds(Part part) const
{
    const int w = tl_.header_width();
    const int row1 = 5;
    const int row2 = std::min(25, getHeight() - 19);
    const int readout_w = 58;
    switch (part) {
        case Part::Name: return {14, row1, w - 14 - readout_w - 4, 16};
        case Part::Readout: return {w - readout_w - 6, row1, readout_w, 16};
        case Part::Mode: return {14, row2, 44, 15};
        case Part::Power: return {62, row2, 17, 15};
        case Part::Hide: return {81, row2, 17, 15};
        case Part::Remove: return {100, row2, 17, 15};
        case Part::Add: return {w - 6 - 21, row2, 21, 15};
    }
    return {};
}

juce::Colour AutomationLaneView::curve_colour() const
{
    juce::Colour c = data_.colour.getBrightness() < 0.45f ? data_.colour.brighter(0.6f) : data_.colour;
    c = c.withSaturation(std::min(1.0f, c.getSaturation() * 1.1f));
    if (!live()) c = colours::text_dim.withAlpha(0.8f);
    return c;
}

bool AutomationLaneView::live() const { return data_.lane && data_.lane->enabled && data_.mode != "off"; }

std::string AutomationLaneView::fresh_label(const std::string& base) const
{
    return tl_.context().session.undo_label() == base ? base + " " : base;
}

bool AutomationLaneView::commit(const std::string& label, std::vector<project::AutomationPoint> pts)
{
    if (!data_.lane) return false;
    if (pts.empty()) return false;
    const auto& now = data_.lane->points;
    bool same = now.size() == pts.size();
    for (std::size_t i = 0; same && i < pts.size(); ++i)
        same = now[i].tick == pts[i].tick && std::abs(now[i].value - pts[i].value) < 1e-12 && now[i].shape == pts[i].shape;
    if (same) return false;
    return tl_.context().session.edit_lane(data_.owner_id, data_.lane->id, label,
                                           [p = std::move(pts)](project::AutomationLane& l) { l.points = p; });
}

juce::String AutomationLaneView::position_text(Ticks t) const
{
    const auto& meter = tl_.context().session.data().meter;
    const int bar = meter.bar_at(t);
    const auto sig = meter.at_bar(bar);
    const Ticks in_bar = t - meter.bar_start(bar);
    const int beat = static_cast<int>(in_bar / sig.beat_ticks()) + 1;
    const int six = static_cast<int>((in_bar % sig.beat_ticks()) * 4 / sig.beat_ticks()) + 1;
    return juce::String(bar) + "." + juce::String(beat) + "." + juce::String(six);
}

// --- Painting ---------------------------------------------------------------------------------------

void AutomationLaneView::paint(juce::Graphics& g)
{
    const auto clip = g.getClipBounds();
    if (clip.getX() < tl_.header_width()) paint_header(g);
    if (clip.getRight() > tl_.header_width()) paint_body(g, clip);
    g.setColour(colours::outline.withAlpha(0.55f));
    g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
}

void AutomationLaneView::paint_header(juce::Graphics& g)
{
    const int w = tl_.header_width(), h = getHeight();
    const auto header = juce::Rectangle<int>(0, 0, w, h);
    g.setColour(colours::panel.darker(0.12f));
    g.fillRect(header);
    // The owner's colour, and a thread tying the lane to its track.
    g.setColour(data_.colour.withAlpha(0.55f));
    g.fillRect(0, 0, 5, h);
    g.setColour(data_.colour.withAlpha(0.3f));
    g.fillRect(8, 0, 1, h);
    g.setColour(colours::outline);
    g.drawVerticalLine(w - 1, 0.0f, static_cast<float>(h));

    auto hovered = [&](Part p) { return hover_part_ && *hover_part_ == p; };
    auto box = [&](Part p, juce::Colour fill, juce::Colour edge) {
        const auto r = part_bounds(p).toFloat();
        g.setColour(fill);
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(edge);
        g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
        return r;
    };

    // Parameter chooser.
    const auto name_r = part_bounds(Part::Name);
    {
        const bool has = data_.lane.has_value();
        juce::String name = has ? juce::String(data_.lane->name) : juce::String("Choose parameter");
        g.setColour(hovered(Part::Name) ? colours::text : (has ? colours::text.withAlpha(0.92f) : colours::accent));
        g.setFont(juce::FontOptions(11.5f, has ? juce::Font::bold : juce::Font::plain));
        const auto text_r = name_r.withTrimmedRight(10);
        g.drawText(name, text_r, juce::Justification::centredLeft, true);
        const float tw = std::min(static_cast<float>(text_r.getWidth()),
                                  juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), name));
        const float ax = static_cast<float>(text_r.getX()) + tw + 5.0f, ay = static_cast<float>(name_r.getCentreY());
        juce::Path tri;
        tri.addTriangle(ax - 3.0f, ay - 1.5f, ax + 3.0f, ay - 1.5f, ax, ay + 2.0f);
        g.setColour(colours::text_dim);
        g.fillPath(tri);
    }
    if (data_.lane) {
        g.setColour(curve_colour());
        g.setFont(juce::FontOptions(10.5f));
        g.drawText(readout(), part_bounds(Part::Readout), juce::Justification::centredRight, false);
    }
    if (h < 30) return;

    // Mode (the owner's), enable, hide, remove, add.
    {
        const auto mc = mode_colour(data_.mode);
        const auto r = box(Part::Mode, mc.withAlpha(hovered(Part::Mode) ? 0.32f : 0.2f), mc.withAlpha(0.75f));
        g.setColour(mc.brighter(0.3f));
        g.setFont(juce::FontOptions(9.5f, juce::Font::bold));
        g.drawText(mode_text(data_.mode), r, juce::Justification::centred, false);
    }
    const auto idle = colours::panel_raised;
    if (data_.lane) {
        const bool on = data_.lane->enabled;
        const auto pr = box(Part::Power, on ? juce::Colour(0xff4fbf73).withAlpha(0.18f) : idle,
                            hovered(Part::Power) ? colours::text_dim : colours::outline);
        draw_icon(g, Part::Power, pr, on ? juce::Colour(0xff6fd98f) : colours::text_dim.withAlpha(0.6f), on);
        for (Part p : {Part::Hide, Part::Remove}) {
            const auto r = box(p, idle, hovered(p) ? colours::text_dim : colours::outline);
            draw_icon(g, p, r, hovered(p) ? colours::text : colours::text_dim, false);
        }
    }
    const auto ar = box(Part::Add, idle, hovered(Part::Add) ? colours::text_dim : colours::outline);
    draw_icon(g, Part::Add, ar, hovered(Part::Add) ? colours::text : colours::text_dim, false);
}

void AutomationLaneView::paint_body(juce::Graphics& g, juce::Rectangle<int> clip)
{
    const auto b = body();
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(b);
    g.setColour(kBodyColour);
    g.fillRect(b.getIntersection(clip));
    tl_.paint_timeline_grid(g, b);

    const float fx0 = static_cast<float>(std::max(b.getX(), clip.getX() - 2));
    const float fx1 = static_cast<float>(std::min(b.getRight(), clip.getRight() + 2));

    // Ruler range.
    const auto range = tl_.selected_range();
    if (range.second > range.first) {
        const float x0 = static_cast<float>(x_of(range.first)), x1 = static_cast<float>(x_of(range.second));
        const juce::Colour c = tl_.range_is_punch() ? colours::playhead : colours::accent;
        g.setColour(c.withAlpha(0.07f));
        g.fillRect(juce::Rectangle<float>(x0, 0.0f, x1 - x0, static_cast<float>(getHeight())));
        g.setColour(c.withAlpha(0.45f));
        g.drawVerticalLine(juce::roundToInt(x0), 0.0f, static_cast<float>(getHeight()));
        g.drawVerticalLine(juce::roundToInt(x1), 0.0f, static_cast<float>(getHeight()));
    }

    if (!data_.lane || !info_) {
        g.setColour(colours::text_dim.withAlpha(0.7f));
        g.setFont(juce::FontOptions(11.0f, juce::Font::italic));
        g.drawText("Choose a parameter in the header to add an automation lane", b.reduced(12, 0),
                   juce::Justification::centredLeft, true);
        return;
    }
    const auto& info = *info_;
    const float top = y_of_norm(1.0), bottom = y_of_norm(0.0);

    // Value grid.
    g.setFont(juce::FontOptions(9.0f));
    for (const auto& line : grid_lines(info, getHeight())) {
        const float y = y_of_value(line.value);
        const bool zero = std::abs(line.value) < 1e-9 && !info.toggle && info.min < 0.0;
        g.setColour(juce::Colours::white.withAlpha(zero ? 0.11f : 0.05f));
        g.drawHorizontalLine(juce::roundToInt(y), fx0, fx1);
        if (clip.getX() <= b.getX() + 40) {
            g.setColour(colours::text_dim.withAlpha(0.65f));
            const float ly = std::clamp(y - 10.0f, 0.0f, static_cast<float>(getHeight()) - 11.0f);
            g.drawText(line.label, juce::Rectangle<float>(static_cast<float>(b.getX()) + 4.0f, ly, 40.0f, 10.0f),
                       juce::Justification::centredLeft, false);
        }
    }

    // The curve, sampled per pixel exactly as value_at plays it, with the
    // corners at breakpoints drawn where they are.
    const auto colour = curve_colour();
    const auto& pts = points();
    if (!pts.empty()) {
        juce::Path line;
        auto y_at = [&](Ticks t) { return y_of_value(*curve_value(t)); };
        line.startNewSubPath(fx0, y_at(tick_of(fx0)));
        std::size_t next = 0;
        while (next < pts.size() && x_of(pts[next].tick) <= fx0) ++next;
        for (float x = std::floor(fx0) + 1.0f; x <= fx1 + 0.5f; x += 1.0f) {
            while (next < pts.size() && x_of(pts[next].tick) <= x) {
                const float px = static_cast<float>(x_of(pts[next].tick));
                if (pts[next].tick > 0 && next > 0) line.lineTo(px, y_at(pts[next].tick - 1));
                line.lineTo(px, y_of_value(pts[next].value));
                ++next;
            }
            line.lineTo(x, y_at(tick_of(x)));
        }
        juce::Path fill(line);
        fill.lineTo(fx1 + 0.5f, bottom + static_cast<float>(kPad));
        fill.lineTo(fx0, bottom + static_cast<float>(kPad));
        fill.closeSubPath();
        g.setGradientFill(juce::ColourGradient(colour.withAlpha(info.toggle ? 0.30f : 0.24f), 0.0f, top,
                                               colour.withAlpha(info.toggle ? 0.22f : 0.04f), 0.0f, bottom, false));
        g.fillPath(fill);
        g.setColour(colour.withAlpha(live() ? 1.0f : 0.7f));
        g.strokePath(line, juce::PathStrokeType(1.6f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));

        // Breakpoints.
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const auto p = position_of(pts[i].tick, pts[i].value);
            if (p.x < fx0 - 8.0f || p.x > fx1 + 8.0f) continue;
            const bool sel = selection_.count(pts[i].tick) > 0;
            const bool hover = static_cast<int>(i) == hover_point_;
            const float r = hover || sel ? kHandle + 1.0f : kHandle;
            const auto e = juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(p);
            g.setColour(sel ? colours::text : kBodyColour);
            g.fillEllipse(e);
            g.setColour(sel ? colour.brighter(0.2f) : colour);
            g.drawEllipse(e, sel ? 1.8f : 1.4f);
        }
    }

    // Pencil stroke in progress.
    if (drag_ == Drag::Pencil && pencil_.size() > 1) {
        juce::Path stroke;
        stroke.startNewSubPath(position_of(pencil_.front().tick, pencil_.front().value));
        for (const auto& p : pencil_) stroke.lineTo(position_of(p.tick, p.value));
        g.setColour(colours::text.withAlpha(0.9f));
        g.strokePath(stroke, juce::PathStrokeType(1.5f));
    }

    // Marquee.
    if (drag_ == Drag::Marquee && !marquee_.isEmpty()) {
        g.setColour(colours::accent.withAlpha(0.14f));
        g.fillRect(marquee_);
        g.setColour(colours::accent.withAlpha(0.8f));
        g.drawRect(marquee_, 1.0f);
    }

    // Lane off or mode off: say so.
    if (!live()) {
        g.setColour(colours::text_dim.withAlpha(0.85f));
        g.setFont(juce::FontOptions(9.5f, juce::Font::bold));
        const juce::String why = !data_.lane->enabled ? "LANE OFF" : "AUTOMATION OFF";
        g.drawText(why, b.withTrimmedRight(8).withHeight(14).translated(0, 2), juce::Justification::centredRight, false);
    }

    // Playhead and the value it is reading.
    const float px = static_cast<float>(x_of(tl_.playhead_tick()));
    if (px >= b.getX() && px <= b.getRight()) {
        g.setColour(colours::playhead);
        g.fillRect(juce::Rectangle<float>(px - 0.5f, 0.0f, 1.5f, static_cast<float>(getHeight())));
        if (const auto v = curve_value(tl_.playhead_tick()); v && !pts.empty()) {
            const float y = y_of_value(*v);
            g.setColour(colours::playhead);
            g.fillEllipse(px - 2.5f, y - 2.5f, 5.0f, 5.0f);
        }
    }

    // Value bubble while dragging.
    if (bubble_.isNotEmpty()) {
        g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
        const float tw = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), bubble_) + 12.0f;
        auto r = juce::Rectangle<float>(tw, 17.0f).withCentre(bubble_at_.translated(0.0f, -16.0f));
        if (r.getY() < 1.0f) r.setY(bubble_at_.y + 9.0f);
        r.setX(std::clamp(r.getX(), static_cast<float>(b.getX()) + 2.0f, static_cast<float>(b.getRight()) - r.getWidth() - 2.0f));
        g.setColour(juce::Colour(0xf0101215));
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(colour.withAlpha(0.9f));
        g.drawRoundedRectangle(r, 4.0f, 1.0f);
        g.setColour(colours::text);
        g.drawText(bubble_, r, juce::Justification::centred, false);
    }
}

// --- Mouse ----------------------------------------------------------------------------------------

void AutomationLaneView::mouseMove(const juce::MouseEvent& e)
{
    auto cursor = juce::MouseCursor::NormalCursor;
    std::optional<Part> part;
    int hover = -1;
    if (e.y >= getHeight() - kResizeZone) {
        cursor = juce::MouseCursor::UpDownResizeCursor;
    } else if (e.x < tl_.header_width()) {
        for (Part p : {Part::Name, Part::Mode, Part::Power, Part::Hide, Part::Remove, Part::Add})
            if (part_bounds(p).contains(e.getPosition()) && (data_.lane || p == Part::Name || p == Part::Mode || p == Part::Add))
                part = p;
        if (part) cursor = juce::MouseCursor::PointingHandCursor;
    } else if (data_.lane) {
        hover = point_at(e.position);
        if (hover >= 0) cursor = juce::MouseCursor::DraggingHandCursor;
        else if (tl_.pencil_mode()) cursor = juce::MouseCursor::CrosshairCursor;
        else if (const auto v = curve_value(tick_of(e.position.x)); v && std::abs(y_of_value(*v) - e.position.y) <= 5.0f)
            cursor = juce::MouseCursor::PointingHandCursor;
    }
    setMouseCursor(cursor);
    if (part != hover_part_) {
        hover_part_ = part;
        repaint(0, 0, tl_.header_width(), getHeight());
    }
    if (hover != hover_point_) {
        auto dirty = [&](int i) {
            if (i < 0 || i >= static_cast<int>(points().size())) return;
            const auto& p = points()[static_cast<std::size_t>(i)];
            repaint(juce::Rectangle<float>(16.0f, 16.0f).withCentre(position_of(p.tick, p.value)).toNearestInt());
        };
        dirty(hover_point_);
        hover_point_ = hover;
        dirty(hover_point_);
    }
}

void AutomationLaneView::mouseExit(const juce::MouseEvent&)
{
    if (hover_part_ || hover_point_ >= 0) {
        hover_part_.reset();
        hover_point_ = -1;
        repaint();
    }
}

void AutomationLaneView::begin_points_drag(Ticks anchor, const std::string& label, bool moved)
{
    drag_ = Drag::Points;
    drag_label_ = label;
    moved_ = moved;
    origin_ = work_.points;
    origin_selection_ = selection_;
    anchor_tick_ = anchor;
    anchor_norm_ = 0.0;
    for (const auto& p : origin_)
        if (p.tick == anchor && info_) anchor_norm_ = to_norm(*info_, p.value);
}

void AutomationLaneView::mouseDown(const juce::MouseEvent& e)
{
    if (editor_) close_editor(true);
    grabKeyboardFocus();
    drag_ = Drag::None;
    bubble_.clear();
    if (e.y >= getHeight() - kResizeZone && !e.mods.isPopupMenu()) {
        drag_ = Drag::Resize;
        resize_start_ = getHeight();
        return;
    }
    if (e.x < tl_.header_width()) {
        header_click(e);
        return;
    }
    if (!data_.lane || !info_) {
        show_chooser(false);
        return;
    }
    const int index = point_at(e.position);
    if (e.mods.isPopupMenu()) {
        if (index >= 0) show_point_menu(index);
        else show_segment_menu(e.position);
        return;
    }
    if (index >= 0) {
        const Ticks t = points()[static_cast<std::size_t>(index)].tick;
        if (e.mods.isShiftDown()) {
            if (!selection_.erase(t)) selection_.insert(t);
            repaint();
            if (!selection_.count(t)) return;
        } else if (!selection_.count(t)) {
            selection_ = {t};
        }
        begin_points_drag(t, fresh_label("Move automation"), false);
        bubble_ = format_value(*info_, points()[static_cast<std::size_t>(index)].value) + "  " + position_text(t);
        bubble_at_ = position_of(t, points()[static_cast<std::size_t>(index)].value);
        repaint();
        return;
    }
    if (tl_.pencil_mode()) {
        drag_ = Drag::Pencil;
        pencil_ = {{tick_of(e.position.x), value_of_y(e.position.y), "linear"}};
        return;
    }
    // On the line: a new breakpoint on the curve, dragged straight away.
    const Ticks t = tick_of(e.position.x);
    if (const auto v = curve_value(t); v && !e.mods.isShiftDown() && std::abs(y_of_value(*v) - e.position.y) <= 5.0f) {
        const Ticks at = e.mods.isCommandDown() ? t : tl_.snap_tick(t);
        const double value = *curve_value(at);
        // Keep the shape of the segment it splits.
        std::string shape = "linear";
        for (const auto& p : work_.points)
            if (p.tick <= at) shape = p.shape;
        work_.points.push_back({at, value, shape});
        project::normalise(work_);
        selection_ = {at};
        begin_points_drag(at, fresh_label("Add automation point"), true);
        bubble_ = format_value(*info_, value) + "  " + position_text(at);
        bubble_at_ = position_of(at, value);
        repaint();
        return;
    }
    // Elsewhere: a marquee (a click adds a point at the mouse).
    drag_ = Drag::Marquee;
    marquee_ = {};
    origin_selection_ = e.mods.isShiftDown() ? selection_ : std::set<Ticks>{};
    if (!e.mods.isShiftDown() && !selection_.empty()) {
        selection_.clear();
        repaint();
    }
}

void AutomationLaneView::mouseDrag(const juce::MouseEvent& e)
{
    switch (drag_) {
        case Drag::Resize: {
            const int h = std::clamp(resize_start_ + e.getDistanceFromDragStartY(), kMinHeight, kMaxHeight);
            if (data_.lane) tl_.set_lane_height(data_.owner_id, data_.lane->id, h);
            return;
        }
        case Drag::Points: {
            if (!info_) return;
            if (!moved_ && e.getDistanceFromDragStart() < 3) return;
            moved_ = true;
            // Horizontal: the anchor snaps to the grid (Cmd/Ctrl: free), the
            // selection moves together and never crosses a point left behind.
            Ticks dt = 0;
            if (!e.mods.isShiftDown()) {
                const double raw = static_cast<double>(anchor_tick_) + tl_.tick_at_x(e.position.x) -
                                   tl_.tick_at_x(static_cast<float>(e.mouseDownPosition.x));
                const Ticks t = std::max<Ticks>(0, static_cast<Ticks>(std::llround(raw)));
                dt = (e.mods.isCommandDown() ? t : tl_.snap_tick(t)) - anchor_tick_;
                Ticks lo = -anchor_tick_, hi = std::numeric_limits<Ticks>::max() / 4;
                for (std::size_t i = 0; i < origin_.size(); ++i) {
                    if (!origin_selection_.count(origin_[i].tick)) continue;
                    lo = std::max(lo, -origin_[i].tick);
                    for (std::size_t j = i; j-- > 0;)
                        if (!origin_selection_.count(origin_[j].tick)) {
                            lo = std::max(lo, origin_[j].tick + 1 - origin_[i].tick);
                            break;
                        }
                    for (std::size_t j = i + 1; j < origin_.size(); ++j)
                        if (!origin_selection_.count(origin_[j].tick)) {
                            hi = std::min(hi, origin_[j].tick - 1 - origin_[i].tick);
                            break;
                        }
                }
                dt = lo > hi ? 0 : std::clamp(dt, lo, hi);
            }
            // Vertical: in lane space, so a fader-law or log lane moves as it looks.
            const double dn_raw = norm_of_y(e.position.y) - norm_of_y(static_cast<float>(e.mouseDownPosition.y));
            const double anchor_n = std::clamp(anchor_norm_ + dn_raw, 0.0, 1.0);
            const double dn = anchor_n - anchor_norm_;
            std::vector<project::AutomationPoint> out;
            std::set<Ticks> sel;
            double anchor_value = 0.0;
            for (auto p : origin_) {
                if (origin_selection_.count(p.tick)) {
                    const bool is_anchor = p.tick == anchor_tick_;
                    p.tick += dt;
                    p.value = from_norm(*info_, std::clamp(to_norm(*info_, p.value) + dn, 0.0, 1.0));
                    sel.insert(p.tick);
                    if (is_anchor) anchor_value = p.value;
                }
                out.push_back(p);
            }
            work_.points = std::move(out);
            project::normalise(work_);
            selection_ = std::move(sel);
            bubble_ = format_value(*info_, anchor_value) + "  " + position_text(anchor_tick_ + dt);
            bubble_at_ = position_of(anchor_tick_ + dt, anchor_value);
            repaint();
            return;
        }
        case Drag::Marquee: {
            marquee_ = juce::Rectangle<float>(e.mouseDownPosition, e.position);
            std::set<Ticks> sel = origin_selection_;
            for (const auto& p : points())
                if (marquee_.contains(position_of(p.tick, p.value))) sel.insert(p.tick);
            selection_ = std::move(sel);
            repaint();
            return;
        }
        case Drag::Pencil: {
            const Ticks t = tick_of(e.position.x);
            const double v = value_of_y(e.position.y);
            // Drawing back over the stroke redraws it.
            while (!pencil_.empty() && pencil_.back().tick >= t && pencil_.size() > 1) pencil_.pop_back();
            if (pencil_.empty() || t > pencil_.back().tick) pencil_.push_back({t, v, "linear"});
            else pencil_.back().value = v;
            repaint();
            return;
        }
        case Drag::None: return;
    }
}

void AutomationLaneView::mouseUp(const juce::MouseEvent& e)
{
    const Drag drag = drag_;
    drag_ = Drag::None;
    bubble_.clear();
    switch (drag) {
        case Drag::Points:
            if (moved_ && !commit(drag_label_, work_.points)) work_ = data_.lane ? *data_.lane : work_;
            repaint();
            return;
        case Drag::Marquee:
            if (marquee_.getWidth() < 3.0f && marquee_.getHeight() < 3.0f && info_ && !e.mods.isShiftDown()) {
                // A click in empty space: a breakpoint at the mouse.
                const Ticks t = tick_of(e.position.x);
                const Ticks at = e.mods.isCommandDown() ? t : tl_.snap_tick(t);
                const double v = value_of_y(e.position.y);
                auto pts = work_.points;
                std::string shape = "linear";
                for (const auto& p : pts)
                    if (p.tick <= at) shape = p.shape;
                pts.push_back({at, v, shape});
                project::AutomationLane l = work_;
                l.points = pts;
                project::normalise(l);
                if (commit(fresh_label("Add automation point"), l.points)) selection_ = {at};
            }
            marquee_ = {};
            repaint();
            return;
        case Drag::Pencil:
            commit_pencil();
            repaint();
            return;
        case Drag::Resize:
        case Drag::None: return;
    }
}

void AutomationLaneView::commit_pencil()
{
    if (!info_ || pencil_.empty()) return;
    auto drawn = pencil_;
    pencil_.clear();
    if (drawn.size() == 1) {
        auto pts = work_.points;
        const Ticks at = tl_.snap_tick(drawn.front().tick);
        pts.push_back({at, drawn.front().value, "linear"});
        project::AutomationLane l = work_;
        l.points = pts;
        project::normalise(l);
        if (commit(fresh_label("Add automation point"), l.points)) selection_ = {at};
        return;
    }
    if (info_->toggle) {
        // Switches: only where the drawn state changes.
        std::vector<project::AutomationPoint> steps;
        for (auto p : drawn) {
            p.value = p.value >= 0.5 ? 1.0 : 0.0;
            p.shape = "step";
            if (steps.empty() || std::abs(steps.back().value - p.value) > 0.5) steps.push_back(p);
        }
        drawn = std::move(steps);
    } else {
        const double tol = info_->unit == "dB" ? 0.25 : (info_->max - info_->min) * 0.004;
        project::thin(drawn, tol);
    }
    project::AutomationLane l = work_;
    const Ticks from = drawn.front().tick, to = drawn.back().tick;
    project::write_pass(l, from, to, std::move(drawn));
    selection_.clear();
    commit(fresh_label("Draw automation"), l.points);
}

void AutomationLaneView::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (e.x < tl_.header_width() || !data_.lane) return;
    const int index = point_at(e.position);
    if (index >= 0) edit_value(points()[static_cast<std::size_t>(index)].tick);
}

// --- Header ----------------------------------------------------------------------------------------

void AutomationLaneView::header_click(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) {
        show_header_menu();
        return;
    }
    const auto p = e.getPosition();
    auto& s = tl_.context().session;
    if (part_bounds(Part::Mode).contains(p)) show_mode_menu();
    else if (part_bounds(Part::Add).contains(p)) show_chooser(false);
    else if (part_bounds(Part::Name).contains(p)) show_chooser(data_.lane.has_value());
    else if (data_.lane && part_bounds(Part::Power).contains(p)) {
        const bool on = !data_.lane->enabled;
        s.edit_lane(data_.owner_id, data_.lane->id, on ? "Enable automation lane" : "Disable automation lane",
                    [on](project::AutomationLane& l) { l.enabled = on; });
    } else if (data_.lane && part_bounds(Part::Hide).contains(p)) {
        s.edit_lane(data_.owner_id, data_.lane->id, fresh_label("Hide automation lane"),
                    [](project::AutomationLane& l) { l.shown = false; });
    } else if (data_.lane && part_bounds(Part::Remove).contains(p)) {
        confirm_remove();
    }
}

void AutomationLaneView::show_header_menu()
{
    juce::PopupMenu menu;
    menu.addItem(data_.lane ? "Change parameter..." : "Choose parameter...", [this] { show_chooser(data_.lane.has_value()); });
    menu.addItem("Add lane...", [this] { show_chooser(false); });
    juce::PopupMenu modes;
    for (const auto& m : project::automation_modes()) {
        juce::PopupMenu::Item item(mode_text(m));
        item.setColour(mode_colour(m)).setTicked(m == data_.mode).setAction([this, m] {
            tl_.context().session.set_automation_mode(data_.owner_id, m);
        });
        modes.addItem(item);
    }
    menu.addSubMenu("Mode (" + data_.owner_name + ")", modes);
    if (data_.lane) {
        menu.addSeparator();
        menu.addItem(data_.lane->enabled ? "Disable lane" : "Enable lane", [this] {
            const bool on = !data_.lane->enabled;
            tl_.context().session.edit_lane(data_.owner_id, data_.lane->id,
                                            on ? "Enable automation lane" : "Disable automation lane",
                                            [on](project::AutomationLane& l) { l.enabled = on; });
        });
        menu.addItem("Hide lane", [this] {
            tl_.context().session.edit_lane(data_.owner_id, data_.lane->id, fresh_label("Hide automation lane"),
                                            [](project::AutomationLane& l) { l.shown = false; });
        });
        const auto range = tl_.selected_range();
        menu.addItem("Clear automation in range", range.second > range.first, false,
                     [this, range] { clear_range(range.first, range.second); });
        menu.addSeparator();
        menu.addItem("Remove lane...", [this] { confirm_remove(); });
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

void AutomationLaneView::show_mode_menu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader("Automation mode - " + data_.owner_name);
    const std::map<std::string, const char*> help = {
        {"off", "Lanes ignored, controls free"},
        {"read", "Lanes play, controls follow"},
        {"touch", "Writes while a control is held"},
        {"latch", "Writes from the first touch until stop"},
        {"write", "Writes every lane for the whole pass"}};
    for (const auto& m : project::automation_modes()) {
        juce::PopupMenu::Item item(mode_text(m) + "   " + help.at(m));
        item.setColour(mode_colour(m)).setTicked(m == data_.mode).setAction([this, m] {
            tl_.context().session.set_automation_mode(data_.owner_id, m);
        });
        menu.addItem(item);
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(
        localAreaToGlobal(part_bounds(Part::Mode))));
}

void AutomationLaneView::show_chooser(bool replace)
{
    const std::string owner = data_.owner_id;
    const std::string current = data_.lane ? data_.lane->id : std::string{};
    const std::string current_param = data_.lane ? data_.lane->param : std::string{};
    auto& ctx = tl_.context();
    automation_ui::show_param_menu(ctx, owner, this, [&ctx, owner, current, current_param, replace](const std::string& param,
                                                                                               const std::string& name) {
        if (replace && param == current_param) return;
        ctx.session.ensure_lane(owner, param, name);
        // Choosing in a lane's header shows the other parameter in its place.
        if (replace && !current.empty())
            ctx.session.edit_lane(owner, current, "Hide automation lane", [](project::AutomationLane& l) { l.shown = false; });
    });
}

void AutomationLaneView::confirm_remove()
{
    if (!data_.lane) return;
    const std::string owner = data_.owner_id, lane = data_.lane->id;
    auto& ctx = tl_.context();
    if (data_.lane->points.size() <= 1) {
        ctx.session.remove_lane(owner, lane);
        return;
    }
    auto options = juce::MessageBoxOptions::makeOptionsOkCancel(
        juce::MessageBoxIconType::QuestionIcon, "Remove Automation Lane",
        "Remove the " + juce::String(data_.lane->name) + " lane of " + data_.owner_name + " and its " +
            juce::String(static_cast<int>(data_.lane->points.size())) + " breakpoints?\n(Undo brings it back.)",
        "Remove", "Cancel", this);
    juce::AlertWindow::showAsync(options, [&ctx, owner, lane](int result) {
        if (result == 1) ctx.session.remove_lane(owner, lane);
    });
}

// --- Point menus and typing -----------------------------------------------------------------------

void AutomationLaneView::show_point_menu(int index)
{
    if (!info_) return;
    const auto p = points()[static_cast<std::size_t>(index)];
    std::set<Ticks> targets = selection_.count(p.tick) ? selection_ : std::set<Ticks>{p.tick};
    if (!selection_.count(p.tick)) {
        selection_ = {p.tick};
        repaint();
    }
    const int n = static_cast<int>(targets.size());
    juce::PopupMenu menu;
    menu.addSectionHeader(n > 1 ? juce::String(n) + " points"
                                : format_value(*info_, p.value) + " at " + position_text(p.tick));
    if (!info_->toggle) {
        for (const char* shape : {"linear", "step", "smooth"}) {
            bool all = true;
            for (const auto& q : points())
                if (targets.count(q.tick)) all = all && q.shape == shape;
            const juce::String label = juce::String(shape).substring(0, 1).toUpperCase() + juce::String(shape).substring(1);
            menu.addItem(label, true, all, [this, targets, s = std::string(shape)] { set_shape(targets, s); });
        }
        menu.addSeparator();
    }
    menu.addItem("Edit value...", n == 1, false, [this, t = p.tick] { edit_value(t); });
    menu.addItem(n > 1 ? "Copy points" : "Copy point", [this] { copy_selection(); });
    menu.addItem(n > 1 ? "Delete points" : "Delete point", [this] { delete_selection(); });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

void AutomationLaneView::show_segment_menu(juce::Point<float> at)
{
    if (!info_) return;
    const Ticks t = tick_of(at.x);
    int left = -1;
    for (std::size_t i = 0; i < points().size(); ++i)
        if (points()[i].tick <= t) left = static_cast<int>(i);
    juce::PopupMenu menu;
    if (left >= 0 && left + 1 < static_cast<int>(points().size()) && !info_->toggle) {
        const auto lp = points()[static_cast<std::size_t>(left)];
        menu.addSectionHeader("Segment from " + position_text(lp.tick));
        for (const char* shape : {"linear", "step", "smooth"}) {
            const juce::String label = juce::String(shape).substring(0, 1).toUpperCase() + juce::String(shape).substring(1);
            menu.addItem(label, true, lp.shape == shape,
                         [this, tick = lp.tick, s = std::string(shape)] { set_shape({tick}, s); });
        }
        menu.addSeparator();
    }
    const Ticks snapped = tl_.snap_tick(t);
    menu.addItem("Add point here", [this, snapped, v = value_of_y(at.y)] {
        auto pts = work_.points;
        std::string shape = "linear";
        for (const auto& p : pts)
            if (p.tick <= snapped) shape = p.shape;
        pts.push_back({snapped, v, shape});
        project::AutomationLane l = work_;
        l.points = pts;
        project::normalise(l);
        commit(fresh_label("Add automation point"), l.points);
    });
    menu.addItem("Select all points", [this] {
        std::set<Ticks> all;
        for (const auto& p : points()) all.insert(p.tick);
        set_selection(all);
    });
    menu.addItem("Delete selected points", !selection_.empty(), false, [this] { delete_selection(); });
    menu.addItem("Paste at playhead", !clipboard().points.empty(), false, [this] { paste_at(tl_.playhead_tick()); });
    const auto range = tl_.selected_range();
    menu.addItem("Clear automation in range", range.second > range.first, false,
                 [this, range] { clear_range(range.first, range.second); });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

void AutomationLaneView::edit_value(Ticks tick)
{
    if (!info_) return;
    const project::AutomationPoint* point = nullptr;
    for (const auto& p : points())
        if (p.tick == tick) point = &p;
    if (!point) return;
    selection_ = {tick};
    editor_tick_ = tick;
    editor_ = std::make_unique<juce::TextEditor>("value");
    auto& ed = *editor_;
    addAndMakeVisible(ed);
    ed.setFont(juce::FontOptions(12.0f));
    ed.setJustification(juce::Justification::centred);
    ed.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff101215));
    ed.setColour(juce::TextEditor::outlineColourId, curve_colour());
    ed.setColour(juce::TextEditor::focusedOutlineColourId, curve_colour());
    ed.setColour(juce::TextEditor::textColourId, colours::text);
    ed.setText(format_value(*info_, point->value), false);
    const auto p = position_of(point->tick, point->value);
    auto r = juce::Rectangle<int>(84, 20).withCentre(p.toInt().translated(0, p.y > 24.0f ? -14 : 14));
    r.setX(std::clamp(r.getX(), body().getX() + 2, std::max(body().getX() + 2, getWidth() - r.getWidth() - 2)));
    r.setY(std::clamp(r.getY(), 0, std::max(0, getHeight() - r.getHeight())));
    ed.setBounds(r);
    ed.selectAll();
    ed.onReturnKey = [this] { close_editor(true); };
    ed.onEscapeKey = [this] { close_editor(false); };
    ed.onFocusLost = [this] { close_editor(true); };
    ed.grabKeyboardFocus();
    repaint();
}

void AutomationLaneView::close_editor(bool commit_text)
{
    if (!editor_) return;
    const juce::String text = editor_->getText();
    // The editor may be calling us: delete it once its callback has returned.
    auto* dying = editor_.release();
    dying->setVisible(false);
    juce::MessageManager::callAsync([dying] { delete dying; });
    if (commit_text && info_)
        if (const auto v = parse_value(*info_, text)) set_point_value(editor_tick_, *v);
    grabKeyboardFocus();
}

// --- Edits ----------------------------------------------------------------------------------------

void AutomationLaneView::set_selection(std::set<Ticks> ticks)
{
    selection_.clear();
    for (const auto& p : points())
        if (ticks.count(p.tick)) selection_.insert(p.tick);
    repaint();
}

bool AutomationLaneView::delete_selection()
{
    if (selection_.empty() || !data_.lane) return false;
    std::vector<project::AutomationPoint> kept;
    for (const auto& p : work_.points)
        if (!selection_.count(p.tick)) kept.push_back(p);
    // A lane always keeps one point (an empty lane would play nothing).
    if (kept.empty() && !work_.points.empty()) {
        auto first = work_.points.front();
        kept.push_back(first);
    }
    const int n = static_cast<int>(selection_.size());
    selection_.clear();
    return commit(fresh_label(n == 1 ? "Delete automation point" : "Delete automation points"), kept);
}

bool AutomationLaneView::clear_range(Ticks from, Ticks to)
{
    if (!data_.lane || to <= from) return false;
    const auto before = project::value_at(work_, from);
    const auto after = project::value_at(work_, to);
    if (!before || !after) return false;
    std::vector<project::AutomationPoint> out;
    for (const auto& p : work_.points)
        if (p.tick < from || p.tick > to) out.push_back(p);
    out.push_back({from, *before, "linear"});
    out.push_back({to, *after, "linear"});
    project::AutomationLane l = work_;
    l.points = out;
    project::normalise(l);
    selection_.clear();
    return commit(fresh_label("Clear automation in range"), l.points);
}

bool AutomationLaneView::set_shape(const std::set<Ticks>& targets, const std::string& shape)
{
    auto pts = work_.points;
    for (auto& p : pts)
        if (targets.count(p.tick)) p.shape = shape;
    return commit(fresh_label("Automation shape"), pts);
}

bool AutomationLaneView::set_point_value(Ticks tick, double value)
{
    auto pts = work_.points;
    for (auto& p : pts)
        if (p.tick == tick) p.value = value;
    return commit(fresh_label("Automation value"), pts);
}

void AutomationLaneView::copy_selection() const
{
    if (selection_.empty() || !info_) return;
    auto& c = clipboard();
    c = {};
    Ticks first = -1;
    for (const auto& p : points()) {
        if (!selection_.count(p.tick)) continue;
        if (first < 0) first = p.tick;
        c.points.push_back({p.tick - first, to_norm(*info_, p.value)});
        c.shapes.push_back(p.shape);
        c.length = p.tick - first;
    }
    tl_.context().status(juce::String(static_cast<int>(c.points.size())) + " automation points copied");
}

bool AutomationLaneView::paste_at(Ticks tick)
{
    const auto& c = clipboard();
    if (c.points.empty() || !info_ || !data_.lane) return false;
    std::vector<project::AutomationPoint> pasted;
    for (std::size_t i = 0; i < c.points.size(); ++i)
        pasted.push_back({tick + c.points[i].first, from_norm(*info_, c.points[i].second), c.shapes[i]});
    project::AutomationLane l = work_;
    if (pasted.size() == 1) {
        l.points.push_back(pasted.front());
        project::normalise(l);
    } else {
        // Replaces what was there, like a written pass (no thinning of what was copied).
        std::vector<project::AutomationPoint> out;
        const auto after = project::value_at(work_, tick + c.length);
        for (const auto& p : work_.points)
            if (p.tick < tick || p.tick > tick + c.length + 1) out.push_back(p);
        out.insert(out.end(), pasted.begin(), pasted.end());
        if (after) out.push_back({tick + c.length + 1, *after, "linear"});
        l.points = out;
        project::normalise(l);
    }
    std::set<Ticks> sel;
    for (const auto& p : pasted) sel.insert(p.tick);
    const bool ok = commit(fresh_label("Paste automation"), l.points);
    if (ok) selection_ = sel;
    return ok;
}

// --- Keys -----------------------------------------------------------------------------------------

bool AutomationLaneView::keyPressed(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) {
        if (!selection_.empty()) return delete_selection() || true;
        const auto range = tl_.selected_range();
        if (range.second > range.first) return clear_range(range.first, range.second) || true;
        return false;
    }
    if (key == juce::KeyPress::escapeKey) {
        if (drag_ != Drag::None) {
            drag_ = Drag::None;
            work_ = data_.lane ? *data_.lane : project::AutomationLane{};
            bubble_.clear();
        }
        selection_.clear();
        repaint();
        return true;
    }
    if (!mods.isAnyModifierKeyDown() && (key.getKeyCode() == 'P' || key.getKeyCode() == 'p')) {
        tl_.set_pencil_mode(!tl_.pencil_mode());
        return true;
    }
    if (mods.isCommandDown() && (key.getKeyCode() == 'A' || key.getKeyCode() == 'a')) {
        std::set<Ticks> all;
        for (const auto& p : points()) all.insert(p.tick);
        set_selection(all);
        return true;
    }
    if (mods.isCommandDown() && (key.getKeyCode() == 'C' || key.getKeyCode() == 'c') && !selection_.empty()) {
        copy_selection();
        return true;
    }
    if (mods.isCommandDown() && (key.getKeyCode() == 'V' || key.getKeyCode() == 'v') && !clipboard().points.empty()) {
        paste_at(tl_.playhead_tick());
        return true;
    }
    return false;
}

void AutomationLaneView::focusLost(FocusChangeType)
{
    if (hover_point_ >= 0) {
        hover_point_ = -1;
        repaint();
    }
}

}  // namespace vcc::desktop
