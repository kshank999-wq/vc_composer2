#include "ui/console/ConsoleStrip.h"

#include <algorithm>
#include <map>
#include <cmath>
#include <type_traits>

#include "audio/PlaybackEngine.h"
#include "ui/Theme.h"
#include "ui/TrackPanel.h"
#include "vcc/project/Automation.h"

namespace vcc::desktop::console {

namespace {

constexpr int kHeading = 12;
constexpr int kRow = 35;  // printed label + knob
constexpr int kLabel = 9;
constexpr int kKnob = 26;
constexpr int kSendRow = 30;
constexpr int kInsertSlot = 15;
constexpr int kDelayRow = 16;
constexpr int kAutoRow = 20;

/// The device rate (for showing delays in milliseconds); 48k without a device.
double engine_rate(PlaybackEngine& engine)
{
    if (auto* device = engine.devices().getCurrentAudioDevice()) {
        const double rate = device->getCurrentSampleRate();
        if (rate > 0.0) return rate;
    }
    return 48000.0;
}

const juce::Colour kGroupColour{0xff5b8fd6};
const juce::Colour kFxColour{0xffa07ad6};
const juce::Colour kMasterColour{0xffd0d4da};

Knob::Spec db_knob(double lo, double hi, double def, juce::Colour cap, bool small = false)
{
    Knob::Spec s;
    s.min = lo;
    s.max = hi;
    s.def = def;
    s.bipolar = lo < 0.0 && hi > 0.0;
    s.cap = cap;
    s.small = small;
    s.text = [](double v) { return db_text(v); };
    return s;
}

Knob::Spec freq_knob(double lo, double hi, double def, juce::Colour cap, bool small = true)
{
    Knob::Spec s;
    s.min = lo;
    s.max = hi;
    s.def = def;
    s.law = Knob::Law::Log;
    s.cap = cap;
    s.small = small;
    s.text = [](double v) { return freq_text(v); };
    return s;
}

Knob::Spec ms_knob(double lo, double hi, double def, juce::Colour cap)
{
    Knob::Spec s;
    s.min = lo;
    s.max = hi;
    s.def = def;
    s.law = Knob::Law::Log;
    s.cap = cap;
    s.small = true;
    s.text = [](double v) { return ms_text(v); };
    return s;
}

Knob::Spec q_knob(juce::Colour cap)
{
    Knob::Spec s;
    s.min = 0.4;
    s.max = 4.0;
    s.def = 1.0;
    s.law = Knob::Law::Log;
    s.cap = cap;
    s.small = true;
    s.text = [](double v) { return "Q " + juce::String(v, 2); };
    return s;
}

Knob::Spec ratio_knob()
{
    Knob::Spec s;
    s.min = 1.0;
    s.max = 20.0;
    s.def = 3.0;
    s.law = Knob::Law::Log;
    s.cap = paint::cap_comp;
    s.text = [](double v) { return juce::String(v, 1) + ":1"; };
    return s;
}

Knob::Spec range_knob()
{
    Knob::Spec s;
    s.min = 0.0;
    s.max = 80.0;
    s.def = 40.0;
    s.cap = paint::cap_gate;
    s.small = true;
    s.text = [](double v) { return juce::String(juce::roundToInt(v)) + " dB range"; };
    return s;
}

Knob::Spec pan_knob()
{
    Knob::Spec s;
    s.min = -1.0;
    s.max = 1.0;
    s.def = 0.0;
    s.bipolar = true;
    s.cap = paint::cap_pan;
    s.text = [](double v) { return "Pan " + pan_text(v); };
    return s;
}

Knob::Spec send_knob()
{
    Knob::Spec s;
    s.min = kMinDb;
    s.max = kMaxDb;
    s.def = 0.0;
    s.law = Knob::Law::Fader;
    s.cap = paint::cap_send;
    s.text = [](double v) { return "Send " + db_text(v); };
    return s;
}

juce::String insert_name(const project::InsertSlot& s)
{
    if (s.kind == "plugin") return s.plugin.name.empty() ? juce::String(s.plugin_ref) : juce::String(s.plugin.name);
    static const std::vector<std::pair<std::string, const char*>> names{
        {"eq", "EQ"}, {"compressor", "Compressor"}, {"gate", "Gate"}, {"reverb", "Reverb"},
        {"delay", "Delay"}, {"limiter", "Limiter"}};
    for (const auto& [k, n] : names)
        if (k == s.kind) return n;
    return juce::String(s.kind).toUpperCase();
}

const std::vector<std::pair<std::string, const char*>>& builtin_inserts()
{
    static const std::vector<std::pair<std::string, const char*>> list{{"eq", "EQ"},           {"compressor", "Compressor"},
                                                                       {"gate", "Gate"},       {"limiter", "Limiter"},
                                                                       {"reverb", "Reverb"},   {"delay", "Delay"}};
    return list;
}

void screw(juce::Graphics& g, float cx, float cy)
{
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.fillEllipse(cx - 3.0f, cy - 2.4f, 6.0f, 6.0f);
    juce::ColourGradient grad(juce::Colour(0xffa8adb4), cx - 1.5f, cy - 2.5f, juce::Colour(0xff3b3e43), cx + 1.5f,
                              cy + 2.5f, false);
    g.setGradientFill(grad);
    g.fillEllipse(cx - 2.6f, cy - 2.6f, 5.2f, 5.2f);
    g.setColour(juce::Colours::black.withAlpha(0.7f));
    g.drawLine(cx - 1.8f, cy + 0.9f, cx + 1.8f, cy - 0.9f, 0.9f);
}

}  // namespace

// --- Layout ----------------------------------------------------------------------------------

const char* section_title(Section s)
{
    switch (s) {
        case Section::Input: return "INPUT";
        case Section::Filters: return "FILTERS";
        case Section::Eq: return "EQUALISER";
        case Section::Dynamics: return "DYNAMICS";
        case Section::Inserts: return "INSERTS";
        case Section::Sends: return "SENDS";
        case Section::Output: return "OUTPUT";
    }
    return "";
}

int ConsoleLayout::section_height(Section s) const noexcept
{
    switch (s) {
        case Section::Input: return kHeading + 19 + kRow + kDelayRow + 2;
        case Section::Filters: return kHeading + kRow + 4;
        case Section::Eq: return kHeading + 33 + 4 * kRow + 20 + 2;
        case Section::Dynamics: return kHeading + 2 * kRow + 14 + kRow + 20 + 2;
        case Section::Inserts: return kHeading + InsertRack::kSlots * kInsertSlot + 5;
        case Section::Sends: return kHeading + juce::jmax(1, fx_count) * kSendRow + 4;
        case Section::Output: return kHeading + 18 + 6;
    }
    return 0;
}

std::vector<ConsoleLayout::Placed> ConsoleLayout::placed() const
{
    std::vector<Placed> out;
    int y = kTopMargin;
    for (int i = 0; i < kSectionCount; ++i) {
        if (!show[static_cast<std::size_t>(i)]) continue;
        const auto s = static_cast<Section>(i);
        out.push_back({s, y, section_height(s)});
        y += section_height(s);
    }
    return out;
}

int ConsoleLayout::top_height() const
{
    int h = kTopMargin + 4;
    for (const auto& p : placed()) h += p.height;
    return h;
}

// --- Panels ----------------------------------------------------------------------------------

struct Strip::Panel final : juce::Component {
    Panel(Strip& s, bool is_top) : strip(s), top(is_top) {}
    void paint(juce::Graphics& g) override
    {
        if (top) strip.paint_top(g, getLocalBounds());
        else strip.paint_bottom(g, getLocalBounds());
    }
    void resized() override
    {
        if (top) strip.layout_top();
        else strip.layout_bottom();
    }
    void mouseDown(const juce::MouseEvent& e) override { strip.panel_mouse_down(e.getEventRelativeTo(this)); }
    Strip& strip;
    bool top;
};

struct Strip::SendRow {
    explicit SendRow(FxBus b)
        : bus(std::move(b)),
          knob(send_knob()),
          pre("PRE", paint::led_amber, "Pre-fader send"),
          mute("M", paint::led_red, "Mute this send")
    {
    }
    FxBus bus;
    Knob knob;
    LedButton pre, mute;
    bool exists = false;
    juce::Rectangle<float> name_area;
};

// --- Strip -----------------------------------------------------------------------------------

Strip::Strip(AppContext& context, Kind kind, std::string id, const ConsoleLayout& layout, std::vector<FxBus> fx_buses,
             int number)
    : context_(context),
      kind_(kind),
      id_(std::move(id)),
      layout_(layout),
      fx_buses_(std::move(fx_buses)),
      trim_(db_knob(-20.0, 20.0, 0.0, paint::cap_grey)),
      phase_(juce::String(juce::CharPointer_UTF8("\xc3\x98")), paint::led_amber, "Polarity invert"),
      hpf_(freq_knob(20.0, 400.0, 80.0, paint::cap_filter, false)),
      lpf_(freq_knob(3000.0, 20000.0, 12000.0, paint::cap_filter, false)),
      hpf_in_("HP", paint::led_blue, "High-pass filter in"),
      lpf_in_("LP", paint::led_blue, "Low-pass filter in"),
      hf_gain_(db_knob(-18.0, 18.0, 0.0, paint::cap_hf)),
      hf_freq_(freq_knob(1500.0, 16000.0, 8000.0, paint::cap_hf.darker(0.35f))),
      hmf_gain_(db_knob(-18.0, 18.0, 0.0, paint::cap_hmf)),
      hmf_freq_(freq_knob(600.0, 7000.0, 2500.0, paint::cap_hmf.darker(0.35f))),
      hmf_q_(q_knob(paint::cap_hmf.darker(0.6f))),
      lmf_gain_(db_knob(-18.0, 18.0, 0.0, paint::cap_lmf)),
      lmf_freq_(freq_knob(200.0, 2500.0, 400.0, paint::cap_lmf.darker(0.35f))),
      lmf_q_(q_knob(paint::cap_lmf.darker(0.6f))),
      lf_gain_(db_knob(-18.0, 18.0, 0.0, paint::cap_lf)),
      lf_freq_(freq_knob(30.0, 450.0, 100.0, paint::cap_lf.darker(0.2f))),
      hf_bell_("BELL", paint::led_white, "HF band: bell instead of shelf"),
      lf_bell_("BELL", paint::led_white, "LF band: bell instead of shelf"),
      eq_in_("EQ IN", paint::led_green, "Equaliser in"),
      comp_threshold_(db_knob(-50.0, 0.0, -18.0, paint::cap_comp)),
      comp_ratio_(ratio_knob()),
      comp_makeup_(db_knob(0.0, 24.0, 0.0, paint::cap_comp)),
      comp_attack_(ms_knob(0.1, 100.0, 10.0, paint::cap_comp.darker(0.3f))),
      comp_release_(ms_knob(10.0, 2000.0, 120.0, paint::cap_comp.darker(0.3f))),
      gate_threshold_(db_knob(-80.0, 0.0, -50.0, paint::cap_gate)),
      gate_range_(range_knob()),
      gate_release_(ms_knob(10.0, 2000.0, 150.0, paint::cap_gate.darker(0.3f))),
      dyn_in_("DYN", paint::led_yellow, "Compressor in"),
      dyn_pre_("PRE", paint::led_white, "Dynamics before the EQ"),
      gate_in_("GATE IN", paint::led_amber, "Gate / expander in"),
      pan_(pan_knob()),
      auto_mode_("Automation mode: Off / Read / Touch / Latch / Write - click to choose; alt-click sets every strip"),
      rec_("REC", paint::led_red, "Record arm"),
      monitor_("AUTO", paint::led_green, "Input monitoring: Auto, Input or Off"),
      mute_("MUTE", paint::led_amber, "Mute"),
      solo_("SOLO", paint::led_green, "Solo"),
      meter_(2, kind == Kind::Master)
{
    number_ = number;
    top_ = std::make_unique<Panel>(*this, true);
    bottom_ = std::make_unique<Panel>(*this, false);
    for (const auto& b : fx_buses_)
        if (b.id != id_) sends_.push_back(std::make_unique<SendRow>(b));
    setup_controls();
    refresh();
}

Strip::~Strip()
{
    for (const auto& param : touching_) context_.engine.end_touch(id_, param);
}

juce::Component& Strip::top() noexcept { return *top_; }
juce::Component& Strip::bottom() noexcept { return *bottom_; }

bool Strip::can_record() const noexcept
{
    return is_track() && (track_kind_ == project::TrackKind::Audio || track_kind_ == project::TrackKind::Reference);
}

template <typename F> void Strip::edit(const juce::String& what, F&& change)
{
    const std::string label = what.toStdString() + " " + name_ + gesture_suffix();
    if (is_track()) context_.session.update_track_mix(id_, label, [&](project::Track& t) { change(t); });
    else context_.session.update_bus(id_, label, [&](project::Bus& b) { change(b); });
}

std::string Strip::engine_id() const { return kind_ == Kind::Master ? std::string("bus-master") : id_; }

void Strip::bind(juce::Component& control, ControlType type, std::string param)
{
    bindings_.push_back({&control, type, std::move(param), false});
}

void Strip::bind_knob(Knob& knob, const juce::String& what, const std::string& param, std::function<void(double)> write)
{
    knob.on_start = [this, param] { start_touch(param); };
    knob.on_change = [this, param, write = std::move(write)](double v) {
        if (!touch(param, v)) write(v);
    };
    knob.on_end = [this, param] { end_touch(param); };
    knob.on_menu = [this, &knob, param] { show_automation_menu(knob, param); };
    knob.setTooltip(what + " - drag; shift for fine; double-click to reset; right-click for automation");
    bind(knob, ControlType::Knob, param);
}

void Strip::bind_toggle(LedButton& button, const std::string& param, std::function<void(bool)> write)
{
    button.onClick = [this, &button, param, write = std::move(write)] {
        begin_gesture();
        const bool v = !button.getToggleState();
        if (start_touch(param)) {
            // A switch is a touch of its own: begin, the new value, end.
            touch(param, v ? 1.0 : 0.0);
            end_touch(param);
            button.set_lit(v);
            return;
        }
        write(v);
    };
    button.on_menu = [this, &button, param] { show_automation_menu(button, param); };
    bind(button, ControlType::Button, param);
}

void Strip::bind_strip_knob(Knob& knob, const juce::String& what, double project::ChannelStrip::*field,
                            const std::string& param)
{
    bind_knob(knob, what, param, [this, what, field](double v) { edit(what, [&](auto& x) { x.strip.*field = v; }); });
}

void Strip::bind_band_knob(Knob& knob, const juce::String& what, project::EqBand project::ChannelStrip::*band,
                           double project::EqBand::*field, const std::string& param)
{
    bind_knob(knob, what, param,
              [this, what, band, field](double v) { edit(what, [&](auto& x) { (x.strip.*band).*field = v; }); });
}

void Strip::bind_strip_button(LedButton& button, const juce::String& what, bool project::ChannelStrip::*field,
                              const std::string& param)
{
    auto write = [this, what, field](bool v) { edit(what, [&](auto& x) { x.strip.*field = v; }); };
    if (param.empty()) {
        button.onClick = [&button, write] {
            begin_gesture();
            write(!button.getToggleState());
        };
        return;
    }
    bind_toggle(button, param, write);
}

void Strip::bind_band_button(LedButton& button, const juce::String& what, project::EqBand project::ChannelStrip::*band,
                             bool project::EqBand::*field)
{
    button.onClick = [this, &button, what, band, field] {
        begin_gesture();
        const bool v = !button.getToggleState();
        edit(what, [&](auto& x) { (x.strip.*band).*field = v; });
    };
}

void Strip::setup_controls()
{
    using CS = project::ChannelStrip;
    using EB = project::EqBand;
    auto& T = *top_;
    auto& B = *bottom_;
    for (juce::Component* c : std::initializer_list<juce::Component*>{
             &input_, &trim_, &phase_, &hpf_, &lpf_, &hpf_in_, &lpf_in_, &curve_, &hf_gain_, &hf_freq_, &hmf_gain_,
             &hmf_freq_, &hmf_q_, &lmf_gain_, &lmf_freq_, &lmf_q_, &lf_gain_, &lf_freq_, &hf_bell_, &lf_bell_, &eq_in_,
             &comp_threshold_, &comp_ratio_, &comp_makeup_, &comp_attack_, &comp_release_, &gate_threshold_,
             &gate_range_, &gate_release_, &dyn_in_, &dyn_pre_, &gate_in_, &gr_, &inserts_, &output_, &delay_})
        T.addChildComponent(c);
    for (auto& row : sends_) {
        T.addChildComponent(row->knob);
        T.addChildComponent(row->pre);
        T.addChildComponent(row->mute);
    }
    for (juce::Component* c :
         std::initializer_list<juce::Component*>{&pan_, &auto_mode_, &mute_, &fader_, &meter_, &readout_, &scribble_})
        B.addAndMakeVisible(c);
    B.addChildComponent(rec_);
    B.addChildComponent(monitor_);
    B.addChildComponent(solo_);

    bind_strip_knob(trim_, "Trim", &CS::trim_db, "strip.trim_db");
    bind_strip_button(phase_, "Phase", &CS::phase_invert, "strip.phase_invert");
    bind_strip_knob(hpf_, "HPF", &CS::hpf_hz, "strip.hpf_hz");
    bind_strip_knob(lpf_, "LPF", &CS::lpf_hz, "strip.lpf_hz");
    bind_strip_button(hpf_in_, "HPF in", &CS::hpf_on, "strip.hpf_on");
    bind_strip_button(lpf_in_, "LPF in", &CS::lpf_on, "strip.lpf_on");
    bind_band_knob(hf_gain_, "HF gain", &CS::hf, &EB::gain_db, "strip.hf.gain_db");
    bind_band_knob(hf_freq_, "HF freq", &CS::hf, &EB::freq, "strip.hf.freq");
    bind_band_knob(hmf_gain_, "HMF gain", &CS::hmf, &EB::gain_db, "strip.hmf.gain_db");
    bind_band_knob(hmf_freq_, "HMF freq", &CS::hmf, &EB::freq, "strip.hmf.freq");
    bind_band_knob(hmf_q_, "HMF Q", &CS::hmf, &EB::q, "strip.hmf.q");
    bind_band_knob(lmf_gain_, "LMF gain", &CS::lmf, &EB::gain_db, "strip.lmf.gain_db");
    bind_band_knob(lmf_freq_, "LMF freq", &CS::lmf, &EB::freq, "strip.lmf.freq");
    bind_band_knob(lmf_q_, "LMF Q", &CS::lmf, &EB::q, "strip.lmf.q");
    bind_band_knob(lf_gain_, "LF gain", &CS::lf, &EB::gain_db, "strip.lf.gain_db");
    bind_band_knob(lf_freq_, "LF freq", &CS::lf, &EB::freq, "strip.lf.freq");
    bind_band_button(hf_bell_, "HF bell", &CS::hf, &EB::bell);  // shelf/bell is not automatable
    bind_band_button(lf_bell_, "LF bell", &CS::lf, &EB::bell);
    bind_strip_button(eq_in_, "EQ in", &CS::eq_on, "strip.eq_on");
    bind_strip_knob(comp_threshold_, "Comp threshold", &CS::comp_threshold_db, "strip.comp_threshold_db");
    bind_strip_knob(comp_ratio_, "Comp ratio", &CS::comp_ratio, "strip.comp_ratio");
    bind_strip_knob(comp_makeup_, "Comp make-up", &CS::comp_makeup_db, "strip.comp_makeup_db");
    bind_strip_knob(comp_attack_, "Comp attack", &CS::comp_attack_ms, "strip.comp_attack_ms");
    bind_strip_knob(comp_release_, "Comp release", &CS::comp_release_ms, "strip.comp_release_ms");
    bind_strip_knob(gate_threshold_, "Gate threshold", &CS::gate_threshold_db, "strip.gate_threshold_db");
    bind_strip_knob(gate_range_, "Gate range", &CS::gate_range_db, "strip.gate_range_db");
    bind_strip_knob(gate_release_, "Gate release", &CS::gate_release_ms, "strip.gate_release_ms");
    bind_strip_button(dyn_in_, "Dynamics in", &CS::comp_on, "strip.comp_on");
    bind_strip_button(dyn_pre_, "Dynamics pre EQ", &CS::dynamics_pre_eq, {});  // not automatable
    bind_strip_button(gate_in_, "Gate in", &CS::gate_on, "strip.gate_on");

    input_.on_click = nullptr;
    output_.on_click = [this] { show_output_menu(); };
    output_.setTooltip("Output routing");
    if (kind_ == Kind::Master) output_.on_click = nullptr;
    inserts_.on_click = [this](int i, bool lamp, bool popup) {
        // A filled slot opens its window; an empty one (or a right-click)
        // offers what can go there; the lamp bypasses.
        if (!lamp && !popup && i < insert_count() && context_.show_insert_window) {
            context_.show_insert_window(id_, i);
            return;
        }
        show_insert_menu(i, lamp);
    };
    inserts_.setTooltip("Inserts - click a processor to open its window; click an empty slot to add one; "
                        "right-click for options; click a lamp to bypass");

    for (auto& row : sends_) {
        auto* r = row.get();
        bind_knob(r->knob, "Send", "send:" + r->bus.id, [this, r](double v) {
            edit("Send " + juce::String(r->bus.name), [&](auto& x) {
                auto it = std::find_if(x.sends.begin(), x.sends.end(), [&](const project::Send& s) { return s.bus_id == r->bus.id; });
                if (it == x.sends.end()) {
                    x.sends.emplace_back(r->bus.id, v);
                } else {
                    it->level_db = v;
                }
            });
        });
        r->knob.on_menu = [this, r] { show_send_menu(r->bus.id); };
        r->knob.setTooltip("Send to " + juce::String(r->bus.name) + " - drag to set (creates the send); right-click for options");
        auto toggle = [this, r](bool pre) {
            begin_gesture();
            const bool v = !(pre ? r->pre : r->mute).getToggleState();
            edit((pre ? "Send pre " : "Send mute ") + juce::String(r->bus.name), [&](auto& x) {
                auto it = std::find_if(x.sends.begin(), x.sends.end(), [&](const project::Send& s) { return s.bus_id == r->bus.id; });
                if (it == x.sends.end()) {
                    x.sends.emplace_back(r->bus.id, kMinDb);
                    it = std::prev(x.sends.end());
                }
                (pre ? it->pre_fader : it->mute) = v;
            });
        };
        r->pre.onClick = [toggle] { toggle(true); };
        r->mute.onClick = [toggle] { toggle(false); };
    }

    bind_knob(pan_, kind_ == Kind::Master ? "Balance" : "Pan", "pan", [this](double v) { edit("Pan", [&](auto& x) { x.pan = v; }); });
    pan_.setTooltip(kind_ == Kind::Master ? "Balance - double-click to centre; right-click for automation"
                                          : "Pan - double-click to centre; right-click for automation");
    fader_.on_start = [this] { start_touch("volume"); };
    fader_.on_change = [this](double v) {
        if (!touch("volume", v)) edit("Fader", [&](auto& x) { x.gain_db = v; });
    };
    fader_.on_end = [this] { end_touch("volume"); };
    fader_.on_menu = [this] { show_automation_menu(fader_, "volume"); };
    bind(fader_, ControlType::Fader, "volume");
    fader_.setTooltip("Level - drag (shift for fine); double-click or alt-click for 0 dB; right-click for automation");
    bind_toggle(mute_, "mute", [this](bool v) { edit(v ? "Mute" : "Unmute", [&](auto& x) { x.mute = v; }); });
    auto_mode_.onClick = [this] { show_automation_mode_menu(juce::ModifierKeys::currentModifiers.isAltDown()); };
    auto_mode_.on_menu = [this] { show_automation_mode_menu(false); };
    solo_.onClick = [this] {
        begin_gesture();
        const bool v = !solo_.getToggleState();
        edit(v ? "Solo" : "Unsolo", [&](auto& x) { x.solo = v; });
    };
    rec_.onClick = [this] { context_.session.set_record_armed(id_, !record_armed_); };
    monitor_.onClick = [this] { show_monitor_menu(); };
    readout_.on_click = [this] { edit_gain_text(); };
    readout_.setTooltip("Level in dB - click to type a value");
    scribble_.on_mouse_down = [this](const juce::MouseEvent& e) {
        if (is_track()) context_.session.select_track(id_);
        if (e.mods.isPopupMenu()) show_name_menu();
    };
    meter_.setTooltip(kind_ == Kind::Master ? "Master output meter (dBFS) - click to reset the clip lights"
                                            : "Peak meter (dBFS) - click to reset the clip light");

    if (kind_ == Kind::Master) {
        fader_.set_cap_colour(juce::Colour(0xffb3322c));
        mute_.setButtonText("MUTE");
    } else if (kind_ == Kind::Group) {
        fader_.set_cap_colour(juce::Colour(0xff35577f));
    } else if (kind_ == Kind::Fx) {
        fader_.set_cap_colour(juce::Colour(0xff5d4479));
    }
    solo_.setVisible(kind_ != Kind::Master);
}

juce::Colour Strip::panel_colour() const
{
    switch (kind_) {
        case Kind::Group: return paint::group_panel;
        case Kind::Fx: return paint::fx_panel;
        case Kind::Master: return paint::master_panel;
        case Kind::Track: break;
    }
    return paint::track_panel;
}

void Strip::set_number(int n)
{
    number_ = n;
    refresh();
}

void Strip::update_selection()
{
    const bool sel = is_track() && context_.session.selected_track() == id_;
    if (scribble_.isVisible()) scribble_.set_selected(sel);
    top_->repaint(0, 0, top_->getWidth(), 4);
}

void Strip::relayout()
{
    layout_top();
    layout_bottom();
    top_->repaint();
    bottom_->repaint();
}

// --- Refresh ---------------------------------------------------------------------------------

void Strip::refresh()
{
    const auto& d = context_.session.data();
    const project::ChannelStrip* cs = nullptr;
    const std::vector<project::Send>* sends = nullptr;
    const std::vector<project::InsertSlot>* inserts = nullptr;
    double gain = 0.0, pan = 0.0;
    bool mute = false, solo = false, dim = false;
    std::string output;
    juce::String badge, number;
    const std::vector<project::AutomationLane>* lanes = nullptr;
    std::string mode;

    if (is_track()) {
        const auto* t = d.find_track(id_);
        if (!t) return;
        cs = &t->strip;
        sends = &t->sends;
        inserts = &t->inserts;
        gain = t->gain_db;
        pan = t->pan;
        mute = t->mute;
        solo = t->solo;
        output = t->output_bus_id;
        lanes = &t->automation;
        mode = t->automation_mode;
        name_ = t->name;
        colour_ = colour_from_hex(t->color);
        track_kind_ = t->kind;
        dim = !context_.session.audible(*t);
        record_armed_ = t->record_armed;
        stereo_input_ = t->input_stereo;
        number = juce::String(number_);
        switch (t->kind) {
            case project::TrackKind::Audio: badge = "AUDIO"; break;
            case project::TrackKind::Reference: badge = "TEMP"; break;
            case project::TrackKind::MidiOnly: badge = "MIDI"; break;
            case project::TrackKind::Aux: badge = "AUX"; break;
            case project::TrackKind::Instrument:
            case project::TrackKind::Video:
            case project::TrackKind::Folder: break;
        }
        if (can_record()) {
            if (t->input < 0) input_.set("NO INPUT");
            else if (t->input_stereo) input_.set("IN " + juce::String(t->input + 1) + "-" + juce::String(t->input + 2));
            else input_.set("IN " + juce::String(t->input + 1));
            input_.set_dot(juce::Colours::transparentBlack);
            input_.on_click = [this] { show_input_menu(); };
            input_.setTooltip("Record input");
            monitor_.setButtonText(t->monitor == "input" ? "INPUT" : (t->monitor == "off" ? "OFF" : "AUTO"));
            monitor_.set_lit(t->monitor == "input");
            monitor_.set_lamp(t->monitor == "off" ? paint::led_red : paint::led_green);
        } else if (t->kind == project::TrackKind::Aux) {
            input_.set("AUX", paint::print_dim, false);
        } else if (!input_.on_click) {
            input_.on_click = [this] {
                if (has_plugin_ && context_.show_plugin_window) context_.show_plugin_window(id_);
                else context_.status("This track is played by the built-in placeholder; load an instrument plug-in first");
            };
            slow_tick();
        }
        rec_.setVisible(can_record());
        monitor_.setVisible(can_record());
    } else {
        const auto* b = d.find_bus(id_);
        if (!b) return;
        cs = &b->strip;
        sends = &b->sends;
        inserts = &b->inserts;
        gain = b->gain_db;
        pan = b->pan;
        mute = b->mute;
        solo = b->solo;
        output = b->output;
        lanes = &b->automation;
        mode = b->automation_mode;
        name_ = kind_ == Kind::Master ? std::string("MASTER") : b->name;
        const auto fallback = kind_ == Kind::Master ? kMasterColour : (kind_ == Kind::Fx ? kFxColour : kGroupColour);
        colour_ = b->color.empty() ? fallback : colour_from_hex(b->color, fallback);
        if (kind_ == Kind::Group && b->color.empty())
            for (const auto* t : context_.session.music_tracks())
                if (t->output_bus_id == id_) {
                    colour_ = colour_from_hex(t->color, kGroupColour);
                    break;
                }
        badge = kind_ == Kind::Master ? "MAIN" : (kind_ == Kind::Fx ? "FX" : "GROUP");
        input_.set(kind_ == Kind::Master ? "MIX BUS" : (kind_ == Kind::Fx ? "FX RETURN" : "GROUP BUS"),
                   paint::print_dim.brighter(0.3f), false);
    }

    // Input and filters
    trim_.set_value(cs->trim_db);
    phase_.set_lit(cs->phase_invert);
    hpf_.set_value(cs->hpf_hz);
    lpf_.set_value(cs->lpf_hz);
    hpf_in_.set_lit(cs->hpf_on);
    lpf_in_.set_lit(cs->lpf_on);
    // EQ
    hf_gain_.set_value(cs->hf.gain_db);
    hf_freq_.set_value(cs->hf.freq);
    hmf_gain_.set_value(cs->hmf.gain_db);
    hmf_freq_.set_value(cs->hmf.freq);
    hmf_q_.set_value(cs->hmf.q);
    lmf_gain_.set_value(cs->lmf.gain_db);
    lmf_freq_.set_value(cs->lmf.freq);
    lmf_q_.set_value(cs->lmf.q);
    lf_gain_.set_value(cs->lf.gain_db);
    lf_freq_.set_value(cs->lf.freq);
    hf_bell_.set_lit(cs->hf.bell);
    lf_bell_.set_lit(cs->lf.bell);
    eq_in_.set_lit(cs->eq_on);
    const project::EqBand* bands[4] = {&cs->lf, &cs->lmf, &cs->hmf, &cs->hf};
    for (std::size_t i = 0; i < 4; ++i) {
        band_on_[i] = bands[i]->on;
        band_q_[i] = bands[i]->q;
    }
    // Dynamics
    comp_threshold_.set_value(cs->comp_threshold_db);
    comp_ratio_.set_value(cs->comp_ratio);
    comp_makeup_.set_value(cs->comp_makeup_db);
    comp_attack_.set_value(cs->comp_attack_ms);
    comp_release_.set_value(cs->comp_release_ms);
    gate_threshold_.set_value(cs->gate_threshold_db);
    gate_range_.set_value(cs->gate_range_db);
    gate_release_.set_value(cs->gate_release_ms);
    dyn_in_.set_lit(cs->comp_on);
    dyn_pre_.set_lit(cs->dynamics_pre_eq);
    gate_in_.set_lit(cs->gate_on);
    // Inserts
    std::vector<InsertRack::Slot> slots;
    for (std::size_t i = 0; i < inserts->size() && i < static_cast<std::size_t>(InsertRack::kSlots); ++i) {
        const auto& s = (*inserts)[i];
        juce::String n = insert_name(s);
        if (i + 1 == static_cast<std::size_t>(InsertRack::kSlots) && inserts->size() > i + 1)
            n << " +" << static_cast<int>(inserts->size() - i - 1);
        slots.push_back({s.suggestion ? "? " + n : n, s.bypass, s.suggestion, true});
    }
    inserts_.set(std::move(slots));
    // Sends
    bool names_changed = false;
    for (auto& row : sends_) {
        const auto* bus = d.find_bus(row->bus.id);
        if (bus && bus->name != row->bus.name) {
            row->bus.name = bus->name;
            names_changed = true;
        }
        auto it = std::find_if(sends->begin(), sends->end(), [&](const project::Send& s) { return s.bus_id == row->bus.id; });
        row->exists = it != sends->end();
        row->knob.set_dim(!row->exists);
        row->knob.set_value(row->exists ? it->level_db : kMinDb);
        row->pre.set_lit(row->exists && it->pre_fader);
        row->mute.set_lit(row->exists && it->mute);
    }
    if (names_changed) top_->repaint();
    // Output
    if (kind_ == Kind::Master) {
        output_.set("MAIN OUT", paint::print_dim.brighter(0.3f), false);
    } else {
        juce::String out = "MASTER";
        if (const auto* o = d.find_bus(output); o && o->kind != "master") out = juce::String(o->name).toUpperCase();
        output_.set(out);
    }
    // Lower panel
    pan_.set_value(pan);
    if (std::abs(pan - pan_value_) > 1e-9) {
        pan_value_ = pan;
        bottom_->repaint(pan_text_area_);
    }
    mute_.set_lit(mute);
    solo_.set_lit(solo);
    rec_.set_lit(record_armed_);
    fader_.set_value(gain);
    readout_.set(db_text(gain, false), paint::lcd_text, false);
    scribble_.set(name_, colour_, number, badge, dim);
    // Automation: indicators, then whatever the lanes are playing now.
    refresh_automation(*lanes, mode);
    follow_automation();
    update_curve();
    delay_tick();
    update_selection();
}

void Strip::update_curve()
{
    // From the controls, so the curve moves with automated knobs too.
    EqCurveParams ep;
    ep.eq_on = eq_in_.getToggleState();
    ep.hpf_on = hpf_in_.getToggleState();
    ep.lpf_on = lpf_in_.getToggleState();
    ep.hpf = hpf_.value();
    ep.lpf = lpf_.value();
    const std::array<const Knob*, 4> freq{&lf_freq_, &lmf_freq_, &hmf_freq_, &hf_freq_};
    const std::array<const Knob*, 4> gain{&lf_gain_, &lmf_gain_, &hmf_gain_, &hf_gain_};
    const std::array<double, 4> q{band_q_[0], lmf_q_.value(), hmf_q_.value(), band_q_[3]};
    const std::array<bool, 4> bell{lf_bell_.getToggleState(), true, true, hf_bell_.getToggleState()};
    for (std::size_t i = 0; i < 4; ++i)
        ep.bands[i] = {freq[i]->value(), band_on_[i] ? gain[i]->value() : 0.0, q[i], bell[i]};
    curve_.set(ep);
}

// --- Automation ------------------------------------------------------------------------------

void set_all_automation_modes(AppContext& context, const std::string& mode)
{
    auto& session = context.session;
    std::vector<std::string> tracks, buses;  // ids first: every edit replaces the document
    for (const auto* t : session.music_tracks())
        if (t->automation_mode != mode) tracks.push_back(t->id);
    for (const auto& b : session.data().buses)
        if (b.automation_mode != mode) buses.push_back(b.id);
    if (tracks.empty() && buses.empty()) return;
    begin_gesture();
    const std::string label = "Automation " + mode + " (all strips)" + gesture_suffix();  // same label: one undo step
    for (const auto& id : tracks) session.update_track_mix(id, label, [&](project::Track& t) { t.automation_mode = mode; });
    for (const auto& id : buses) session.update_bus(id, label, [&](project::Bus& b) { b.automation_mode = mode; });
}

void Strip::refresh_automation(const std::vector<project::AutomationLane>& lanes, const std::string& mode)
{
    mode_ = mode;
    lanes_.clear();
    for (const auto& l : lanes) lanes_[l.param] = {l.id, l.enabled};
    const bool off = mode_ == "off" || automation_colour(mode_).isTransparent();
    const auto lamp = automation_colour(mode_);
    followers_.clear();
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        auto& b = bindings_[i];
        b.following = false;
        const auto it = lanes_.find(b.param);
        const bool active = !off && it != lanes_.end() && it->second.second;
        const auto colour = active ? lamp : juce::Colours::transparentBlack;
        switch (b.type) {
            case ControlType::Knob: static_cast<Knob*>(b.control)->set_automation(colour); break;
            case ControlType::Fader: static_cast<Fader*>(b.control)->set_automation(colour); break;
            case ControlType::Button: static_cast<LedButton*>(b.control)->set_automation(colour); break;
        }
        if (active) followers_.push_back(i);
    }
    auto_mode_.set_mode(mode_);
}

void Strip::show_control_value(Binding& b, double value)
{
    switch (b.type) {
        case ControlType::Knob: static_cast<Knob*>(b.control)->set_value(value); break;
        case ControlType::Fader: static_cast<Fader*>(b.control)->set_value(value); break;
        case ControlType::Button: static_cast<LedButton*>(b.control)->set_lit(value >= 0.5); break;
    }
    show_dependent(b.param, value);
}

void Strip::show_dependent(const std::string& param, double value)
{
    if (param == "volume") {
        readout_.set(db_text(value, false), paint::lcd_text, false);
    } else if (param == "pan" && std::abs(value - pan_value_) > 1e-9) {
        pan_value_ = value;
        bottom_->repaint(pan_text_area_);
    }
}

void Strip::follow_automation()
{
    if (followers_.empty()) return;
    bool stopped = false, strip_moved = false;
    for (const auto i : followers_) {
        auto& b = bindings_[i];
        if (touching_.count(b.param) != 0) continue;  // the hand wins
        if (const auto v = context_.engine.automated_value(id_, b.param)) {
            show_control_value(b, *v);
            b.following = true;
            if (b.param.rfind("strip.", 0) == 0) strip_moved = true;
        } else if (b.following) {
            stopped = true;  // back to the static value
        }
    }
    if (stopped) refresh();  // re-reads the static values (and follows again where a lane still plays)
    else if (strip_moved) update_curve();
}

bool Strip::start_touch(const std::string& param)
{
    if (!context_.engine.writes_automation(id_, param)) return false;
    context_.engine.begin_touch(id_, param);
    touching_.insert(param);
    return true;
}

bool Strip::touch(const std::string& param, double value)
{
    if (touching_.count(param) == 0) return false;
    context_.engine.touch_value(id_, param, value);
    show_dependent(param, value);
    if (param.rfind("strip.", 0) == 0) update_curve();
    return true;
}

void Strip::end_touch(const std::string& param)
{
    if (touching_.erase(param) != 0) context_.engine.end_touch(id_, param);
}

std::string Strip::param_of(const juce::Component* control) const
{
    for (const auto& b : bindings_)
        if (b.control == control) return b.param;
    return {};
}

juce::Component* Strip::control_for(const std::string& param) const
{
    for (const auto& b : bindings_)
        if (b.param == param) return b.control;
    return nullptr;
}

std::vector<std::pair<juce::Component*, std::string>> Strip::automation_controls() const
{
    std::vector<std::pair<juce::Component*, std::string>> out;
    for (const auto& b : bindings_) out.emplace_back(b.control, b.param);
    return out;
}

void Strip::choose_automation_mode(const std::string& mode, bool all_strips)
{
    if (all_strips) set_all_automation_modes(context_, mode);
    else context_.session.set_automation_mode(id_, mode);  // may rebuild nothing: the strip stays
}

juce::String Strip::lane_name(const std::string& param) const
{
    if (param.rfind("send:", 0) == 0) {
        const auto* b = context_.session.data().find_bus(param.substr(5));
        return "Send " + juce::String(b ? b->name : param.substr(5));
    }
    if (param == "pan" && kind_ == Kind::Master) return "Balance";
    if (const auto info = project::param_info(param)) return info->name;
    return param;
}

void Strip::show_automation_lane(const std::string& param)
{
    if (context_.session.ensure_lane(id_, param, lane_name(param).toStdString()).empty())
        context_.status("This control cannot be automated here");
}

void Strip::toggle_lane_enabled(const std::string& param)
{
    const auto it = lanes_.find(param);
    if (it == lanes_.end()) return;
    begin_gesture();
    const bool on = !it->second.second;
    const std::string lane = it->second.first;
    context_.session.edit_lane(id_, lane,
                               (on ? "Enable automation " : "Disable automation ") + lane_name(param).toStdString() +
                                   gesture_suffix(),
                               [on](project::AutomationLane& l) { l.enabled = on; });
}

void Strip::delay_tick() { delay_.set(context_.engine.latency_samples(engine_id()), engine_rate(context_.engine)); }

void Strip::tick(int tick_count)
{
    const auto m = context_.engine.take_meter(kind_ == Kind::Master ? std::string("bus-master") : id_);
    meter_.push(m.left, m.right, m.clipped);
    gr_.set(m.gain_reduction_db, m.gate_closed);
    follow_automation();
    if (record_armed_ && can_record()) {
        // Armed: steady while recording, blinking while the transport is record-enabled but idle.
        const bool blink = context_.engine.record_enabled() && !context_.engine.recording();
        rec_.set_lit(!blink || (tick_count / 8) % 2 == 0);
    }
}

void Strip::slow_tick()
{
    if (!is_track() || can_record() || track_kind_ == project::TrackKind::Aux) return;
    const auto* t = context_.session.data().find_track(id_);
    if (!t) return;
    const juce::String s = context_.engine.instrument_status(id_);
    juce::Colour c = colours::placeholder;
    if (s.startsWith("Missing")) c = colours::missing;
    else if (t->assignment.status == project::ResolutionStatus::Exact) c = colours::exact;
    else if (t->assignment.status == project::ResolutionStatus::Fallback) c = colours::fallback;
    has_plugin_ = context_.engine.plugin_for(id_) != nullptr;
    input_.set(s, paint::lcd_text, false);
    input_.set_dot(c);
    input_.setTooltip(has_plugin_ ? s + " - click to open the plug-in window" : s);
}

// --- Layout ----------------------------------------------------------------------------------

void Strip::layout_top()
{
    top_labels_.clear();
    placed_ = layout_.placed();
    const float w = static_cast<float>(top_->getWidth());
    const float pad = 3.0f;
    const float colw = (w - 2.0f * pad) / 3.0f;
    const bool narrow = colw < 26.0f;
    auto col = [&](int c, float y, float h, float span = 1.0f) {
        return juce::Rectangle<float>(pad + static_cast<float>(c) * colw, y, colw * span, h);
    };
    auto label = [&](juce::Rectangle<float> r, const juce::String& t, juce::Colour c = paint::print) {
        top_labels_.push_back({r, t, c});
    };
    auto knob_at = [&](Knob& k, int c, float y, const juce::String& text, juce::Colour lc = paint::print) {
        label(col(c, y, static_cast<float>(kLabel)), text, lc);
        k.setBounds(col(c, y + static_cast<float>(kLabel), static_cast<float>(kKnob)).toNearestInt());
        k.setVisible(true);
    };
    auto button_at = [&](juce::Button& b, juce::Rectangle<float> r) {
        b.setBounds(r.toNearestInt());
        b.setVisible(true);
    };
    auto wide = [&](float y, float h) { return juce::Rectangle<float>(pad + colw * 0.5f, y, colw * 2.0f, h); };

    for (int i = 0; i < top_->getNumChildComponents(); ++i) top_->getChildComponent(i)->setVisible(false);

    const auto hf = paint::cap_hf.brighter(0.5f), hmf = paint::cap_hmf.brighter(0.5f), lmf = paint::cap_lmf.brighter(0.6f),
               lf = juce::Colour(0xffc8a27c), comp = paint::cap_comp.brighter(0.3f), gate = paint::cap_gate.brighter(0.3f);
    for (const auto& p : placed_) {
        float y = static_cast<float>(p.y + kHeading);
        switch (p.section) {
            case Section::Input: {
                input_.setBounds(col(0, y, 16.0f, 3.0f).reduced(1.0f, 0.0f).toNearestInt());
                input_.setVisible(true);
                y += 19.0f;
                knob_at(trim_, 0, y, "TRIM");
                label(col(1, y, static_cast<float>(kLabel)), "PHASE");
                button_at(phase_, col(1, y + 13.0f, 16.0f).reduced(narrow ? 1.0f : 3.0f, 0.0f));
                juce::String fmt;
                if (is_track()) {
                    if (can_record()) fmt = stereo_input_ ? (narrow ? "ST" : "STEREO") : "MONO";
                    else if (track_kind_ == project::TrackKind::Instrument || track_kind_ == project::TrackKind::MidiOnly) fmt = "INST";
                } else {
                    fmt = "BUS";
                }
                label(col(2, y + 14.0f, 10.0f), fmt, paint::print_dim);
                y += kRow;
                delay_.setBounds(juce::Rectangle<float>(pad + 1.0f, y + 1.0f, w - 2.0f * pad - 2.0f, 13.0f).toNearestInt());
                delay_.setVisible(true);
                break;
            }
            case Section::Filters:
                knob_at(hpf_, 0, y, "HPF");
                knob_at(lpf_, 2, y, "LPF");
                button_at(hpf_in_, col(1, y + 4.0f, 15.0f).reduced(narrow ? 0.0f : 2.0f, 0.0f));
                button_at(lpf_in_, col(1, y + 20.0f, 15.0f).reduced(narrow ? 0.0f : 2.0f, 0.0f));
                break;
            case Section::Eq:
                curve_.setBounds(col(0, y, 30.0f, 3.0f).reduced(1.0f, 0.0f).toNearestInt());
                curve_.setVisible(true);
                y += 33.0f;
                knob_at(hf_gain_, 0, y, "HF", hf);
                knob_at(hf_freq_, 1, y, "FREQ", hf);
                label(col(2, y, static_cast<float>(kLabel)), "SHAPE", hf);
                button_at(hf_bell_, col(2, y + 13.0f, 16.0f).reduced(narrow ? 0.0f : 2.0f, 0.0f));
                y += kRow;
                knob_at(hmf_gain_, 0, y, "HMF", hmf);
                knob_at(hmf_freq_, 1, y, "FREQ", hmf);
                knob_at(hmf_q_, 2, y, "Q", hmf);
                y += kRow;
                knob_at(lmf_gain_, 0, y, "LMF", lmf);
                knob_at(lmf_freq_, 1, y, "FREQ", lmf);
                knob_at(lmf_q_, 2, y, "Q", lmf);
                y += kRow;
                knob_at(lf_gain_, 0, y, "LF", lf);
                knob_at(lf_freq_, 1, y, "FREQ", lf);
                label(col(2, y, static_cast<float>(kLabel)), "SHAPE", lf);
                button_at(lf_bell_, col(2, y + 13.0f, 16.0f).reduced(narrow ? 0.0f : 2.0f, 0.0f));
                y += kRow;
                button_at(eq_in_, wide(y + 2.0f, 16.0f));
                break;
            case Section::Dynamics:
                knob_at(comp_threshold_, 0, y, "THR", comp);
                knob_at(comp_ratio_, 1, y, "RATIO", comp);
                knob_at(comp_makeup_, 2, y, "GAIN", comp);
                y += kRow;
                knob_at(comp_attack_, 0, y, "ATT", comp);
                knob_at(comp_release_, 1, y, "REL", comp);
                button_at(dyn_in_, col(2, y + 2.0f, 15.0f).reduced(narrow ? 0.0f : 2.0f, 0.0f));
                button_at(dyn_pre_, col(2, y + 19.0f, 15.0f).reduced(narrow ? 0.0f : 2.0f, 0.0f));
                y += kRow;
                label({pad, y, 14.0f, 12.0f}, "GR", comp);
                gr_.setBounds(juce::Rectangle<float>(pad + 15.0f, y + 1.0f, w - 2.0f * pad - 16.0f, 10.0f).toNearestInt());
                gr_.setVisible(true);
                y += 14.0f;
                knob_at(gate_threshold_, 0, y, "GATE", gate);
                knob_at(gate_range_, 1, y, "RANGE", gate);
                knob_at(gate_release_, 2, y, "REL", gate);
                y += kRow;
                button_at(gate_in_, wide(y + 2.0f, 16.0f));
                break;
            case Section::Inserts:
                inserts_.setBounds(juce::Rectangle<float>(pad, y, w - 2.0f * pad,
                                                          static_cast<float>(InsertRack::kSlots * kInsertSlot))
                                       .toNearestInt());
                inserts_.setVisible(true);
                break;
            case Section::Sends:
                if (sends_.empty())
                    label({pad, y, w - 2.0f * pad, static_cast<float>(kSendRow)},
                          kind_ == Kind::Master ? "-" : (layout_.fx_count == 0 ? "NO FX BUSES" : "-"), paint::print_dim);
                for (auto& row : sends_) {
                    row->knob.setBounds(juce::Rectangle<float>(pad, y, colw, 28.0f).toNearestInt());
                    row->knob.setVisible(true);
                    const float rx = pad + colw + 1.0f, rw = w - pad - rx;
                    row->name_area = {rx, y + 1.0f, rw, 11.0f};
                    button_at(row->pre, {rx, y + 13.0f, rw * 0.6f, 15.0f});
                    button_at(row->mute, {rx + rw * 0.6f, y + 13.0f, rw * 0.4f, 15.0f});
                    y += kSendRow;
                }
                break;
            case Section::Output:
                output_.setBounds(col(0, y, 18.0f, 3.0f).reduced(1.0f, 0.0f).toNearestInt());
                output_.setVisible(true);
                break;
        }
    }
}

void Strip::layout_bottom()
{
    const int w = bottom_->getWidth(), h = bottom_->getHeight();
    if (w <= 0 || h <= 0) return;
    const int pad = 3;
    const bool master = kind_ == Kind::Master;
    int y = 4;
    const int pd = juce::jmin(40, w - 30);
    pan_.setBounds(w / 2 - pd / 2, y + kLabel, pd, 34);
    pan_text_area_ = {0, y + kLabel + 34, w, 10};
    y += kLabel + 34 + 12;
    const int auto_label = w < 90 ? 20 : 26;
    auto_label_area_ = {pad, y, auto_label, 17};
    auto_mode_.setBounds(pad + auto_label, y, w - 2 * pad - auto_label, 17);
    y += kAutoRow;
    const int half = (w - 2 * pad - 3) / 2;
    rec_.setBounds(pad, y, half, 19);
    monitor_.setBounds(pad + half + 3, y, w - 2 * pad - half - 3, 19);
    y += 22;
    if (master) {
        mute_.setBounds(pad, y, w - 2 * pad, 21);
    } else {
        mute_.setBounds(pad, y, half, 21);
        solo_.setBounds(pad + half + 3, y, w - 2 * pad - half - 3, 21);
    }
    y += 26;

    const int scribble_h = 40;
    scribble_.setBounds(pad, h - scribble_h - 3, w - 2 * pad, scribble_h);
    readout_.setBounds(pad + 8, scribble_.getY() - 21, w - 2 * pad - 16, 17);
    const int zone_top = y, zone_bottom = readout_.getY() - 4;
    const int zone_h = juce::jmax(40, zone_bottom - zone_top);

    // Scale | fader | meter | meter scale, centred.
    const bool narrow = w < 90;
    const int scale_w = narrow ? 14 : 18;
    const int fader_w = narrow ? 24 : 30;
    const int meter_w = master ? (narrow ? 18 : 24) : (narrow ? 9 : 12);
    const int mscale_w = master ? 16 : (narrow ? 0 : 14);
    const int group = scale_w + fader_w + 3 + meter_w + (mscale_w > 0 ? 2 + mscale_w : 0);
    int x = juce::jmax(pad, (w - group) / 2);
    scale_area_ = {x, zone_top, scale_w, zone_h};
    x += scale_w;
    fader_.setBounds(x, zone_top, fader_w, zone_h);
    x += fader_w + 3;
    const int m_top = zone_top + juce::roundToInt(fader_.y_for(kMaxDb)) - 8;
    const int m_bottom = zone_top + juce::roundToInt(fader_.y_for(kMinDb)) + 2;
    meter_.setBounds(x, m_top, meter_w, m_bottom - m_top);
    x += meter_w + 2;
    meter_scale_area_ = mscale_w > 0 ? juce::Rectangle<int>(x, m_top, mscale_w, m_bottom - m_top) : juce::Rectangle<int>();
}

// --- Painting --------------------------------------------------------------------------------

void Strip::paint_top(juce::Graphics& g, juce::Rectangle<int> bounds)
{
    const auto r = bounds.toFloat();
    const float w = r.getWidth();
    fill_panel_cached(g, bounds, panel_colour());
    // Track colour stripe and the module's screws.
    g.setColour(colour_);
    g.fillRect(0.0f, 0.0f, w, 3.0f);
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillRect(0.0f, 3.0f, w, 1.0f);
    screw(g, 7.0f, 8.0f);
    screw(g, w - 7.0f, 8.0f);
    if (number_ > 0 && is_track())
        print_text(g, juce::String(number_), {14.0f, 3.5f, w - 28.0f, 9.0f}, 8.0f, juce::Justification::centred, paint::print_dim);
    for (const auto& p : placed_) {
        print_heading(g, section_title(p.section), {2.0f, static_cast<float>(p.y) + 1.0f, w - 4.0f, 10.0f},
                      paint::print.withAlpha(0.9f));
        const float bottom = static_cast<float>(p.y + p.height);
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.fillRect(0.0f, bottom - 1.0f, w, 1.0f);
        g.setColour(juce::Colours::white.withAlpha(0.07f));
        g.fillRect(0.0f, bottom, w, 1.0f);
    }
    for (const auto& l : top_labels_) print_text(g, l.text, l.area, 7.5f, juce::Justification::centred, l.colour);
    for (const auto& row : sends_)
        if (row->knob.isVisible()) {
            g.setColour(paint::cap_send.brighter(0.6f).withAlpha(row->exists ? 1.0f : 0.5f));
            g.setFont(print_font(8.0f));
            g.drawFittedText(juce::String(row->bus.name).toUpperCase(), row->name_area.toNearestInt(),
                             juce::Justification::centredLeft, 1, 0.6f);
        }
    // Module edges.
    g.setColour(juce::Colours::white.withAlpha(0.08f));
    g.fillRect(0.0f, 0.0f, 1.0f, r.getHeight());
    g.setColour(juce::Colours::black.withAlpha(0.7f));
    g.fillRect(w - 1.0f, 0.0f, 1.0f, r.getHeight());
    if (is_track() && context_.session.selected_track() == id_) {
        g.setColour(paint::selected);
        g.fillRect(0.0f, 0.0f, w, 3.0f);
    }
}

void Strip::paint_bottom(juce::Graphics& g, juce::Rectangle<int> bounds)
{
    const auto r = bounds.toFloat();
    const float w = r.getWidth();
    fill_panel_cached(g, bounds, panel_colour());
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.fillRect(0.0f, 0.0f, w, 1.0f);
    print_text(g, kind_ == Kind::Master ? "BAL" : "PAN", {0.0f, 3.0f, w, 9.0f}, 7.5f);
    print_text(g, pan_text(pan_value_), pan_text_area_.toFloat(), 8.0f, juce::Justification::centred, paint::print_dim.brighter(0.2f));
    print_text(g, "AUTO", auto_label_area_.toFloat(), 7.0f, juce::Justification::centredLeft, paint::print_dim);
    if (kind_ == Kind::Master)
        print_text(g, "STEREO MASTER", rec_.getBounds().getUnion(monitor_.getBounds()).toFloat(), 8.0f,
                   juce::Justification::centred, paint::print_dim);

    // Printed fader scale.
    const float fy = static_cast<float>(fader_.getY());
    const float sx = static_cast<float>(scale_area_.getX()), sw = static_cast<float>(scale_area_.getWidth());
    for (double db : {12.0, 6.0, 0.0, -5.0, -10.0, -20.0, -30.0, -40.0, -60.0, kMinDb}) {
        const float y = fy + fader_.y_for(db);
        const bool unity = std::abs(db) < 0.01;
        juce::String t = db <= kMinDb ? juce::String(juce::CharPointer_UTF8("\xe2\x88\x9e")) : juce::String(juce::roundToInt(std::abs(db)));
        if (db > 0.0) t = "+" + t;
        print_text(g, t, {sx - 2.0f, y - 5.0f, sw - 2.0f, 10.0f}, unity ? 8.5f : 7.5f, juce::Justification::centredRight,
                   unity ? paint::print : paint::print_dim.brighter(0.2f));
        g.setColour(paint::print.withAlpha(unity ? 0.9f : 0.45f));
        const float x0 = static_cast<float>(fader_.getX()) + 3.0f, x1 = static_cast<float>(fader_.getRight()) - 3.0f;
        g.fillRect(x0, y - 0.5f, (x1 - x0) * 0.5f - 4.0f, unity ? 1.5f : 1.0f);
        g.fillRect(x0 + (x1 - x0) * 0.5f + 4.0f, y - 0.5f, (x1 - x0) * 0.5f - 4.0f, unity ? 1.5f : 1.0f);
    }
    // Printed meter scale.
    if (!meter_scale_area_.isEmpty()) {
        const float my = static_cast<float>(meter_.getY());
        for (double db : {0.0, -3.0, -6.0, -12.0, -18.0, -24.0, -40.0, -60.0}) {
            const float y = my + meter_.y_for(db);
            print_text(g, juce::String(juce::roundToInt(std::abs(db))),
                       {static_cast<float>(meter_scale_area_.getX()), y - 4.5f, static_cast<float>(meter_scale_area_.getWidth()), 9.0f},
                       6.5f, juce::Justification::centredLeft, db >= -3.0 ? paint::led_red.withAlpha(0.8f) : paint::print_dim);
        }
        print_text(g, "CLIP", {static_cast<float>(meter_.getX()) - 6.0f, my - 9.0f, static_cast<float>(meter_.getWidth()) + 12.0f, 8.0f},
                   6.0f, juce::Justification::centred, paint::led_red.withAlpha(0.7f));
    }
    g.setColour(juce::Colours::white.withAlpha(0.08f));
    g.fillRect(0.0f, 0.0f, 1.0f, r.getHeight());
    g.setColour(juce::Colours::black.withAlpha(0.7f));
    g.fillRect(w - 1.0f, 0.0f, 1.0f, r.getHeight());
}

void Strip::panel_mouse_down(const juce::MouseEvent& e)
{
    if (is_track()) context_.session.select_track(id_);
    if (e.mods.isPopupMenu()) show_name_menu();
}

// --- Menus -----------------------------------------------------------------------------------

namespace {
/// Menu callbacks run later; the strip may have been rebuilt by then.
template <typename F> std::function<void(int)> guarded(juce::Component* panel, F f)
{
    return [alive = juce::Component::SafePointer<juce::Component>(panel), f](int r) {
        if (alive) f(r);
    };
}
}  // namespace

void Strip::show_input_menu()
{
    const auto* t = context_.session.data().find_track(id_);
    if (!t) return;
    juce::PopupMenu m;
    const int n = context_.engine.num_inputs();
    m.addItem(1, "No input", true, t->input < 0);
    if (n == 0) m.addSectionHeader("No audio input device is open");
    const int count = n > 0 ? n : 2;
    juce::PopupMenu mono, stereo;
    for (int ch = 0; ch < count; ++ch) {
        juce::String label = "In " + juce::String(ch + 1);
        if (n > 0 && context_.engine.input_name(ch).isNotEmpty()) label << "  (" << context_.engine.input_name(ch) << ")";
        mono.addItem(100 + ch, label, true, t->input == ch && !t->input_stereo);
    }
    for (int ch = 0; ch + 1 < count; ch += 2)
        stereo.addItem(200 + ch, "In " + juce::String(ch + 1) + "-" + juce::String(ch + 2), true, t->input == ch && t->input_stereo);
    m.addSubMenu("Mono", mono);
    m.addSubMenu("Stereo", stereo);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&input_), guarded(top_.get(), [this](int r) {
        if (r <= 0) return;
        begin_gesture();
        edit("Input", [&](auto& x) {
            if constexpr (std::is_same_v<std::decay_t<decltype(x)>, project::Track>) {
                if (r == 1) x.input = -1;
                else if (r >= 200) {
                    x.input = r - 200;
                    x.input_stereo = true;
                } else {
                    x.input = r - 100;
                    x.input_stereo = false;
                }
            }
        });
    }));
}

void Strip::show_output_menu()
{
    const auto& d = context_.session.data();
    std::string current;
    if (is_track()) {
        if (const auto* t = d.find_track(id_)) current = t->output_bus_id;
    } else if (const auto* b = d.find_bus(id_)) {
        current = b->output;
    }
    output_ids_.clear();
    juce::PopupMenu m;
    std::string master_id = "bus-master";
    for (const auto& b : d.buses)
        if (b.kind == "master") master_id = b.id;
    output_ids_.push_back(master_id);
    m.addItem(1, "Master", true, current == master_id || current.empty() || current == "master");
    for (const char* kind : {"group", "fx"}) {
        bool first = true;
        for (const auto& b : d.buses) {
            if (b.kind != kind || b.id == id_ || b.output == id_) continue;
            if (first) m.addSectionHeader(std::string(kind) == "group" ? "Groups" : "FX buses");
            first = false;
            output_ids_.push_back(b.id);
            m.addItem(static_cast<int>(output_ids_.size()), b.name, true, b.id == current);
        }
    }
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&output_), guarded(top_.get(), [this](int r) {
        if (r <= 0 || r > static_cast<int>(output_ids_.size())) return;
        const std::string target = output_ids_[static_cast<std::size_t>(r - 1)];
        begin_gesture();
        if (is_track()) context_.session.set_output_bus(id_, target);
        else context_.session.update_bus(id_, "Output " + name_ + gesture_suffix(), [&](project::Bus& b) { b.output = target; });
    }));
}

void Strip::show_monitor_menu()
{
    const auto* t = context_.session.data().find_track(id_);
    if (!t) return;
    juce::PopupMenu m;
    m.addItem(1, "Auto (input while armed)", true, t->monitor == "auto");
    m.addItem(2, "Input", true, t->monitor == "input");
    m.addItem(3, "Off", true, t->monitor == "off");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&monitor_), guarded(top_.get(), [this](int r) {
        if (r <= 0) return;
        begin_gesture();
        const std::string mode = r == 2 ? "input" : (r == 3 ? "off" : "auto");
        context_.session.update_track_mix(id_, "Monitor " + name_ + gesture_suffix(), [&](project::Track& x) { x.monitor = mode; });
    }));
}

int Strip::insert_count() const
{
    if (is_track()) {
        if (const auto* t = context_.session.data().find_track(id_)) return static_cast<int>(t->inserts.size());
    } else if (const auto* b = context_.session.data().find_bus(id_)) {
        return static_cast<int>(b->inserts.size());
    }
    return 0;
}

void Strip::show_insert_menu(int index, bool lamp)
{
    const std::vector<project::InsertSlot>* list = nullptr;
    if (is_track()) {
        if (const auto* t = context_.session.data().find_track(id_)) list = &t->inserts;
    } else if (const auto* b = context_.session.data().find_bus(id_)) {
        list = &b->inserts;
    }
    if (!list) return;
    const bool used = index < static_cast<int>(list->size());
    const auto at = static_cast<std::size_t>(index);
    if (used && lamp) {
        begin_gesture();
        const bool v = !(*list)[at].bypass;
        edit(v ? "Bypass insert" : "Activate insert", [&](auto& x) {
            if (at < x.inserts.size()) x.inserts[at].bypass = v;
        });
        return;
    }
    // Built-in processors (100+) and the scanned effect plug-ins (1000+),
    // grouped by manufacturer.
    juce::Array<juce::PluginDescription> effects;
    for (const auto& d : context_.plugins.known().getTypes())
        if (!d.isInstrument) effects.add(d);
    std::sort(effects.begin(), effects.end(), [](const juce::PluginDescription& a, const juce::PluginDescription& b) {
        return a.manufacturerName == b.manufacturerName ? a.name < b.name : a.manufacturerName < b.manufacturerName;
    });
    auto fill = [&](juce::PopupMenu& menu) {
        for (std::size_t i = 0; i < builtin_inserts().size(); ++i)
            menu.addItem(100 + static_cast<int>(i), builtin_inserts()[i].second);
        juce::PopupMenu plugins;
        std::map<juce::String, juce::PopupMenu> by_maker;
        for (int i = 0; i < effects.size(); ++i)
            by_maker[effects[i].manufacturerName.isEmpty() ? juce::String("Other") : effects[i].manufacturerName].addItem(
                1000 + i, effects[i].name);
        for (auto& [maker, sub] : by_maker) plugins.addSubMenu(maker, sub);
        if (effects.isEmpty()) plugins.addItem(-1, "No effect plug-ins scanned yet (Tools > Plug-ins)", false);
        menu.addSubMenu("Effect plug-ins", plugins);
    };
    juce::PopupMenu m, add;
    fill(add);
    if (used) {
        const auto& s = (*list)[at];
        m.addSectionHeader(insert_name(s) + (s.suggestion ? " (suggested)" : ""));
        m.addItem(5, "Open window...");
        if (s.suggestion && s.kind != "plugin") m.addItem(3, "Use the built-in " + insert_name(s));
        m.addItem(1, "Bypass", true, s.bypass);
        m.addSubMenu("Replace with", add);
        if (at > 0) m.addItem(4, "Move up");
        m.addSeparator();
        m.addItem(2, "Remove");
    } else {
        m.addSectionHeader("Add insert");
        fill(m);
    }
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&inserts_), guarded(top_.get(), [this, at, used, effects](int r) {
        if (r <= 0) return;
        if (r == 5) {
            if (context_.show_insert_window) context_.show_insert_window(id_, static_cast<int>(at));
            return;
        }
        begin_gesture();
        if (r >= 1000 && r - 1000 < effects.size()) {
            const auto ref = context_.plugins.ref_for(effects[r - 1000]);
            edit(used ? "Replace insert" : "Add insert", [&](auto& x) {
                project::InsertSlot slot;
                slot.kind = "plugin";
                slot.plugin_ref = ref.identifier;
                slot.plugin = ref;
                slot.suggestion = false;
                if (used && at < x.inserts.size()) x.inserts[at] = slot;
                else x.inserts.push_back(slot);
            });
            return;
        }
        if (r >= 100 && r < 1000) {
            const auto& kind = builtin_inserts()[static_cast<std::size_t>(r - 100)];
            edit(used ? "Replace insert" : "Add insert", [&](auto& x) {
                project::InsertSlot slot;
                slot.kind = kind.first;
                slot.suggestion = false;
                if (used && at < x.inserts.size()) x.inserts[at] = slot;
                else x.inserts.push_back(slot);
            });
            return;
        }
        edit(r == 2 ? "Remove insert" : (r == 4 ? "Move insert" : "Insert"), [&](auto& x) {
            if (at >= x.inserts.size()) return;
            if (r == 1) x.inserts[at].bypass = !x.inserts[at].bypass;
            else if (r == 2) x.inserts.erase(x.inserts.begin() + static_cast<std::ptrdiff_t>(at));
            else if (r == 3) x.inserts[at].suggestion = false;
            else if (r == 4 && at > 0) std::swap(x.inserts[at], x.inserts[at - 1]);
        });
    }));
}

void Strip::show_send_menu(const std::string& bus_id)
{
    juce::PopupMenu m;
    const std::string param = "send:" + bus_id;
    m.addSectionHeader(lane_name(param));
    m.addItem(1, "Send at 0 dB");
    m.addItem(2, "Remove send");
    m.addSeparator();
    add_automation_items(m, param);
    m.showMenuAsync(juce::PopupMenu::Options(), guarded(top_.get(), [this, bus_id, param](int r) {
        if (r <= 0 || handle_automation_item(r, param)) return;
        begin_gesture();
        edit(r == 2 ? "Remove send" : "Send", [&](auto& x) {
            auto it = std::find_if(x.sends.begin(), x.sends.end(), [&](const project::Send& s) { return s.bus_id == bus_id; });
            if (r == 2) {
                if (it != x.sends.end()) x.sends.erase(it);
            } else if (it == x.sends.end()) {
                x.sends.emplace_back(bus_id, 0.0);
            } else {
                it->level_db = 0.0;
            }
        });
    }));
}

namespace {
constexpr int kShowLaneItem = 901;
constexpr int kLaneEnabledItem = 902;
constexpr int kModeItem = 910;  // + index into automation_modes()
}  // namespace

void Strip::add_automation_items(juce::PopupMenu& m, const std::string& param) const
{
    m.addItem(kShowLaneItem, "Show automation lane");
    const auto it = lanes_.find(param);
    if (it != lanes_.end()) m.addItem(kLaneEnabledItem, "Automation enabled", true, it->second.second);
    juce::PopupMenu modes;
    const auto& list = project::automation_modes();
    for (std::size_t i = 0; i < list.size(); ++i)
        modes.addItem(kModeItem + static_cast<int>(i), automation_label(list[i]), true, list[i] == mode_);
    m.addSubMenu("Automation mode (" + automation_label(mode_) + ")", modes);
}

bool Strip::handle_automation_item(int result, const std::string& param)
{
    const auto& list = project::automation_modes();
    if (result == kShowLaneItem) show_automation_lane(param);
    else if (result == kLaneEnabledItem) toggle_lane_enabled(param);
    else if (result >= kModeItem && result < kModeItem + static_cast<int>(list.size()))
        choose_automation_mode(list[static_cast<std::size_t>(result - kModeItem)], false);
    else return false;
    return true;
}

void Strip::show_automation_menu(juce::Component& target, const std::string& param)
{
    juce::PopupMenu m;
    m.addSectionHeader(lane_name(param));
    add_automation_items(m, param);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target),
                    guarded(top_.get(), [this, param](int r) { handle_automation_item(r, param); }));
}

void Strip::show_automation_mode_menu(bool all_strips)
{
    juce::PopupMenu m;
    m.addSectionHeader(all_strips ? juce::String("Automation mode - every strip") : "Automation mode - " + juce::String(name_));
    const auto& list = project::automation_modes();
    static const char* const hints[] = {"lanes ignored", "play the lanes", "write while held", "write until stop",
                                        "write the whole pass"};
    for (std::size_t i = 0; i < list.size(); ++i)
        m.addColouredItem(1 + static_cast<int>(i), automation_label(list[i]) + "   " + hints[i],
                          list[i] == "off" ? paint::print_dim : automation_colour(list[i]), true,
                          !all_strips && list[i] == mode_);
    if (!all_strips) {
        m.addSeparator();
        juce::PopupMenu all;
        for (std::size_t i = 0; i < list.size(); ++i) all.addItem(101 + static_cast<int>(i), automation_label(list[i]));
        m.addSubMenu("Every strip", all);
    }
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&auto_mode_),
                    guarded(top_.get(), [this, all_strips](int r) {
                        const auto& modes = project::automation_modes();
                        const int n = static_cast<int>(modes.size());
                        if (r >= 1 && r <= n) choose_automation_mode(modes[static_cast<std::size_t>(r - 1)], all_strips);
                        else if (r >= 101 && r < 101 + n) choose_automation_mode(modes[static_cast<std::size_t>(r - 101)], true);
                    }));
}

void Strip::show_name_menu()
{
    if (is_track()) {
        TrackPanel::show_track_menu(context_, id_);
        return;
    }
    if (kind_ == Kind::Master) return;
    juce::PopupMenu m;
    m.addItem(1, "Rename...");
    m.addItem(2, "Remove bus");
    m.showMenuAsync(juce::PopupMenu::Options(), guarded(top_.get(), [this](int r) {
        if (r == 2) {
            context_.session.remove_bus(id_);  // rebuilds the console: nothing may follow
            return;
        }
        if (r != 1) return;
        auto* w = new juce::AlertWindow("Rename bus", {}, juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("name", juce::String(name_));
        w->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        auto& session = context_.session;
        const std::string id = id_;
        w->enterModalState(true, juce::ModalCallbackFunction::create([w, &session, id](int result) {
                               const auto text = w->getTextEditorContents("name").trim().toStdString();
                               if (result == 1 && !text.empty())
                                   session.update_bus(id, "Rename bus", [&](project::Bus& b) { b.name = text; });
                           }),
                           true);
    }));
}

void Strip::edit_gain_text()
{
    gain_editor_ = std::make_unique<juce::TextEditor>();
    auto& e = *gain_editor_;
    e.setJustification(juce::Justification::centred);
    e.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    e.setColour(juce::TextEditor::backgroundColourId, paint::lcd_back);
    e.setColour(juce::TextEditor::textColourId, paint::lcd_text);
    e.setColour(juce::TextEditor::outlineColourId, paint::selected);
    e.setText(db_text(fader_.value(), false), false);
    e.selectAll();
    bottom_->addAndMakeVisible(e);
    e.setBounds(readout_.getBounds());
    e.grabKeyboardFocus();
    juce::Component::SafePointer<juce::Component> panel(bottom_.get());
    auto close = [this, panel] {
        juce::MessageManager::callAsync([this, panel] {
            if (panel) gain_editor_.reset();
        });
    };
    e.onReturnKey = [this, close] {
        const auto t = gain_editor_->getText().trim().toLowerCase();
        double v = t.startsWith("-inf") || t.startsWith("off") ? kMinDb : t.retainCharacters("+-.0123456789").getDoubleValue();
        v = juce::jlimit(kMinDb, kMaxDb, v);
        begin_gesture();
        if (start_touch("volume")) {
            touch("volume", v);
            end_touch("volume");
            fader_.set_value(v);
        } else {
            edit("Fader", [&](auto& x) { x.gain_db = v; });
        }
        close();
    };
    e.onEscapeKey = close;
    e.onFocusLost = close;
}

}  // namespace vcc::desktop::console
