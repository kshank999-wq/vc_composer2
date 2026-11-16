#include "ui/library/ArticulationMapWindow.h"

#include <algorithm>
#include <map>

#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "ui/library/LibraryBrowser.h"
#include "vcc/project/ArticulationMap.h"

namespace vcc::desktop {

namespace nt = notation;

namespace {

juce::Component::SafePointer<dialogs::ToolWindow>& map_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
ArticulationMapPanel*& map_slot()
{
    static ArticulationMapPanel* p = nullptr;
    return p;
}
juce::Component::SafePointer<dialogs::ToolWindow>& missing_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
juce::Component*& missing_slot()
{
    static juce::Component* p = nullptr;
    return p;
}

const nt::ArticulationAction kActions[] = {
    nt::ArticulationAction::Keyswitch,     nt::ArticulationAction::ControlChange,
    nt::ArticulationAction::ProgramChange, nt::ArticulationAction::Channel,
    nt::ArticulationAction::VelocityRange, nt::ArticulationAction::ScaleVelocity,
    nt::ArticulationAction::ScaleLength,   nt::ArticulationAction::ScaleVelocityAndLength};
const char* kActionNames[] = {"(nothing)",      "Keyswitch",    "Controller",       "Program change", "MIDI channel",
                              "Velocity layer", "Scale velocity", "Scale length", "Velocity + length"};

/// The articulations a map can act on (those that change a sound).
const nt::Articulation kRows[] = {
    nt::Articulation::Legato,   nt::Articulation::Staccato,  nt::Articulation::Staccatissimo, nt::Articulation::Spiccato,
    nt::Articulation::Tenuto,   nt::Articulation::Portato,   nt::Articulation::Accent,        nt::Articulation::Marcato,
    nt::Articulation::Pizzicato, nt::Articulation::Tremolo,  nt::Articulation::Trill,         nt::Articulation::Harmonic,
    nt::Articulation::Mute};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

int action_index(nt::ArticulationAction a)
{
    for (int i = 0; i < 8; ++i)
        if (kActions[i] == a) return i + 1;
    return 0;
}

}  // namespace

// --- A row --------------------------------------------------------------------------------

class ArticulationMapPanel::Row final : public juce::Component {
public:
    Row(ArticulationMapPanel& panel, int row, int written) : panel_(panel), row_(row)
    {
        name_.setText(panel.row_name(row), juce::dontSendNotification);
        name_.setFont(juce::FontOptions(13.0f, row == 0 ? juce::Font::bold : juce::Font::plain));
        name_.setColour(juce::Label::textColourId, colours::text);
        count_.setText(row == 0 ? juce::String("plain notes") : written > 0 ? juce::String(written) + " written" : juce::String(),
                       juce::dontSendNotification);
        count_.setFont(juce::FontOptions(11.0f));
        count_.setColour(juce::Label::textColourId, colours::text_dim);
        action_.setTitle(panel.row_name(row) + " action");
        for (int i = 0; i < 9; ++i) action_.addItem(kActionNames[i], i + 1);
        action_.onChange = [this] { panel_.set_action(row_, action_.getSelectedId() - 1); };
        for (auto* s : {&a_, &b_}) {
            s->setSliderStyle(juce::Slider::IncDecButtons);
            s->setTextBoxStyle(juce::Slider::TextBoxLeft, false, 64, 22);
            s->onValueChange = [this] { panel_.set_values(row_, static_cast<int>(a_.getValue()), static_cast<int>(b_.getValue())); };
        }
        a_.setTitle(panel.row_name(row) + " value");
        b_.setTitle(panel.row_name(row) + " second value");
        learn_.setTitle("Learn " + panel.row_name(row));
        learn_.setTooltip("Press the key, move the controller or send the program on your MIDI keyboard");
        learn_.onClick = [this] { panel_.learn(row_); };
        for (auto* c : std::initializer_list<juce::Component*>{&name_, &count_, &action_, &a_, &b_, &learn_}) addAndMakeVisible(*c);
    }

    void show(const nt::ArticulationRule* rule)
    {
        action_.setSelectedId(rule ? action_index(rule->action) + 1 : 1, juce::dontSendNotification);
        const auto act = rule ? rule->action : nt::ArticulationAction::ScaleVelocity;
        auto set = [](juce::Slider& s, double lo, double hi, double v, bool visible) {
            s.setVisible(visible);
            s.setRange(lo, hi, 1.0);
            s.setValue(v, juce::dontSendNotification);
        };
        a_.textFromValueFunction = nullptr;
        b_.textFromValueFunction = nullptr;
        const bool on = rule != nullptr;
        switch (act) {
            case nt::ArticulationAction::Keyswitch:
                a_.textFromValueFunction = [](double v) {
                    return juce::MidiMessage::getMidiNoteName(static_cast<int>(v), true, true, 3);
                };
                set(a_, 0, 127, rule->keyswitch_note, on);
                set(b_, 1, 127, rule->keyswitch_velocity, on);
                break;
            case nt::ArticulationAction::ControlChange:
                a_.textFromValueFunction = [](double v) { return "CC " + juce::String(static_cast<int>(v)); };
                set(a_, 0, 127, rule->cc_number, on);
                set(b_, 0, 127, rule->cc_value, on);
                break;
            case nt::ArticulationAction::ProgramChange:
                set(a_, 0, 127, rule->program, on);
                set(b_, 0, 1, 0, false);
                break;
            case nt::ArticulationAction::Channel:
                a_.textFromValueFunction = [](double v) { return "Ch " + juce::String(static_cast<int>(v)); };
                set(a_, 1, 16, rule->channel, on);
                set(b_, 0, 1, 0, false);
                break;
            case nt::ArticulationAction::VelocityRange:
                set(a_, 1, 127, rule->velocity_min, on);
                set(b_, 1, 127, rule->velocity_max, on);
                break;
            case nt::ArticulationAction::ScaleVelocity:
            case nt::ArticulationAction::ScaleLength:
            case nt::ArticulationAction::ScaleVelocityAndLength: {
                a_.textFromValueFunction = [](double v) { return juce::String(static_cast<int>(v)) + "%"; };
                b_.textFromValueFunction = a_.textFromValueFunction;
                const bool vel = act != nt::ArticulationAction::ScaleLength;
                set(a_, 10, 400, on ? 100.0 * (vel ? rule->velocity_scale : rule->length_scale) : 100.0, on);
                set(b_, 10, 400, on ? 100.0 * rule->length_scale : 100.0,
                    on && act == nt::ArticulationAction::ScaleVelocityAndLength);
                break;
            }
        }
        a_.updateText();
        b_.updateText();
        const bool learnable = !on || act == nt::ArticulationAction::Keyswitch || act == nt::ArticulationAction::ControlChange ||
                               act == nt::ArticulationAction::ProgramChange;
        learn_.setEnabled(learnable);
        learn_.setButtonText(panel_.learning_row() == row_ ? "Listening..." : "Learn");
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(4, 3);
        name_.setBounds(b.removeFromLeft(120));
        count_.setBounds(b.removeFromLeft(80));
        learn_.setBounds(b.removeFromRight(86));
        b.removeFromRight(6);
        action_.setBounds(b.removeFromLeft(150));
        b.removeFromLeft(6);
        const int w = (b.getWidth() - 6) / 2;
        a_.setBounds(b.removeFromLeft(w));
        b.removeFromLeft(6);
        b_.setBounds(b);
    }
    void paint(juce::Graphics& g) override
    {
        if (row_ == 0) g.fillAll(colours::panel_raised);
        g.setColour(colours::outline);
        g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
    }

private:
    ArticulationMapPanel& panel_;
    int row_;
    juce::Label name_, count_;
    juce::ComboBox action_;
    juce::Slider a_, b_;
    juce::TextButton learn_{"Learn"};
};

// --- Panel ------------------------------------------------------------------------------------

ArticulationMapPanel::ArticulationMapPanel(AppContext& context, std::string track_id)
    : context_(context), track_(std::move(track_id))
{
    setComponentID("articulation_map");
    map_slot() = this;
    const auto* t = context_.session.data().find_track(track_);
    map_ = t ? project::expression_map_for(*t) : nt::make_default_expression_map();
    if (t && !t->articulation_map) map_.name = t->name + " map";
    title_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, colours::text);
    title_.setText("Articulation map - " + juce::String(t ? t->name : std::string("(no track)")), juce::dontSendNotification);
    health_.setFont(juce::FontOptions(12.0f));
    status_.setFont(juce::FontOptions(12.0f));
    status_.setColour(juce::Label::textColourId, colours::text_dim);
    base_.setTitle("Start from");
    base_.addItem("Start from: this track's map", 1);
    int id = 2;
    for (const auto& [key, name] : project::builtin_expression_maps()) base_.addItem("Start from: " + juce::String(name), id++);
    base_.setSelectedId(1, juce::dontSendNotification);
    base_.onChange = [this] {
        const int i = base_.getSelectedId() - 2;
        const auto builtins = project::builtin_expression_maps();
        if (i >= 0 && i < static_cast<int>(builtins.size())) start_from(builtins[static_cast<std::size_t>(i)].first);
    };
    name_.setTitle("Map name");
    name_.setText(map_.name, false);
    name_.onTextChange = [this] { map_.name = name_.getText().trim().toStdString(); };
    dynamics_.setTitle("Dynamics");
    dynamics_.addItem("Dynamics on velocity", 1);
    dynamics_.addItem("Dynamics on a controller", 2);
    dynamics_.addItem("Dynamics on both", 3);
    dynamics_.onChange = [this] {
        map_.dynamic_target = static_cast<nt::DynamicTarget>(dynamics_.getSelectedId() - 1);
        dynamics_cc_.setEnabled(map_.dynamic_target != nt::DynamicTarget::Velocity);
        status_.setText(health(), juce::dontSendNotification);
    };
    dynamics_cc_.setTitle("Dynamics controller");
    dynamics_cc_.setRange(1, 127, 1);
    dynamics_cc_.textFromValueFunction = [](double v) { return "CC " + juce::String(static_cast<int>(v)); };
    dynamics_cc_.onValueChange = [this] { map_.dynamics_cc = static_cast<std::uint8_t>(dynamics_cc_.getValue()); };
    view_.setViewedComponent(&list_, false);
    view_.setScrollBarsShown(true, false);
    apply_.setComponentID("map_apply");
    apply_.onClick = [this] { apply(); };
    save_patch_.onClick = [this] { save_to_patch(); };
    save_patch_.setTooltip("Also keep the map with the track's saved library patch, so templates use it");
    revert_.onClick = [this] {
        if (const auto* tr = context_.session.data().find_track(track_)) map_ = project::expression_map_for(*tr);
        load_rows();
    };
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &health_, &status_, &base_, &name_, &dynamics_,
                                                          &dynamics_cc_, &view_, &apply_, &save_patch_, &revert_})
        addAndMakeVisible(*c);
    load_rows();
    startTimerHz(20);
}

ArticulationMapPanel::~ArticulationMapPanel()
{
    if (learning_ >= 0) context_.engine.cancel_learn();
    if (map_slot() == this) map_slot() = nullptr;
}

void ArticulationMapPanel::start_from(const std::string& builtin)
{
    const auto name = map_.name;
    map_ = project::expression_map_named(builtin);
    map_.name = name;
    load_rows();
}

int ArticulationMapPanel::rows() const { return kRowCount + 1; }

juce::String ArticulationMapPanel::row_name(int row) const
{
    if (row == 0) return "Natural";
    if (row < 1 || row > kRowCount) return {};
    return juce::String(nt::to_string(kRows[row - 1])).substring(0, 1).toUpperCase() +
           juce::String(nt::to_string(kRows[row - 1])).substring(1);
}

void ArticulationMapPanel::load_rows()
{
    rows_.clear();
    std::map<nt::Articulation, int> written;
    if (const auto* t = context_.session.data().find_track(track_))
        if (const auto* v = t->active_version())
            for (std::size_t s = 0; s < v->score.staff_count(); ++s)
                for (const auto& n : v->score.notes(s))
                    for (const auto a : kRows)
                        if (!n.is_rest && nt::has_articulation(n.articulations, a)) ++written[a];
    for (int r = 0; r < rows(); ++r) {
        auto row = std::make_unique<Row>(*this, r, r == 0 ? 0 : written[kRows[r - 1]]);
        row->show(r == 0 ? (map_.has_natural ? &map_.natural : nullptr) : map_.rule_for(kRows[r - 1]));
        list_.addAndMakeVisible(*row);
        rows_.push_back(std::move(row));
    }
    name_.setText(map_.name, false);
    dynamics_.setSelectedId(static_cast<int>(map_.dynamic_target) + 1, juce::dontSendNotification);
    dynamics_cc_.setValue(map_.dynamics_cc, juce::dontSendNotification);
    dynamics_cc_.setEnabled(map_.dynamic_target != nt::DynamicTarget::Velocity);
    health_.setText(health(), juce::dontSendNotification);
    resized();
}

void ArticulationMapPanel::rule_changed(int row, const nt::ArticulationRule* rule)
{
    if (row >= 0 && row < static_cast<int>(rows_.size())) rows_[static_cast<std::size_t>(row)]->show(rule);
    const auto h = health();
    health_.setText(h, juce::dontSendNotification);
}

void ArticulationMapPanel::set_action(int row, int action_index_value)
{
    if (row < 0 || row >= rows()) return;
    if (action_index_value <= 0) {
        if (row == 0) map_.has_natural = false;
        else
            map_.articulations.erase(std::remove_if(map_.articulations.begin(), map_.articulations.end(),
                                                    [&](const nt::ArticulationRule& r) { return r.articulation == kRows[row - 1]; }),
                                     map_.articulations.end());
        rule_changed(row, nullptr);
        return;
    }
    nt::ArticulationRule r = row == 0 ? (map_.has_natural ? map_.natural : nt::ArticulationRule{})
                                      : (map_.rule_for(kRows[row - 1]) ? *map_.rule_for(kRows[row - 1]) : nt::ArticulationRule{});
    r.action = kActions[std::clamp(action_index_value, 1, 8) - 1];
    if (r.action == nt::ArticulationAction::Keyswitch && r.keyswitch_velocity == 0) r.keyswitch_velocity = 100;
    if (r.action == nt::ArticulationAction::Channel && (r.channel < 1 || r.channel > 16)) r.channel = 1;
    if (row == 0) {
        map_.natural = r;
        map_.has_natural = true;
        rule_changed(0, &map_.natural);
    } else {
        r.articulation = kRows[row - 1];
        map_.set_rule(r);
        rule_changed(row, map_.rule_for(kRows[row - 1]));
    }
}

void ArticulationMapPanel::set_values(int row, int a, int b)
{
    nt::ArticulationRule* r = nullptr;
    if (row == 0 && map_.has_natural) r = &map_.natural;
    if (row > 0)
        for (auto& x : map_.articulations)
            if (x.articulation == kRows[row - 1]) r = &x;
    if (!r) return;
    auto u8 = [](int v, int lo, int hi) { return static_cast<std::uint8_t>(std::clamp(v, lo, hi)); };
    switch (r->action) {
        case nt::ArticulationAction::Keyswitch: r->keyswitch_note = u8(a, 0, 127); r->keyswitch_velocity = u8(b, 1, 127); break;
        case nt::ArticulationAction::ControlChange: r->cc_number = u8(a, 0, 127); r->cc_value = u8(b, 0, 127); break;
        case nt::ArticulationAction::ProgramChange: r->program = u8(a, 0, 127); break;
        case nt::ArticulationAction::Channel: r->channel = u8(a, 1, 16); break;
        case nt::ArticulationAction::VelocityRange:
            r->velocity_min = u8(std::min(a, b), 1, 127);
            r->velocity_max = u8(std::max(a, b), 1, 127);
            break;
        case nt::ArticulationAction::ScaleVelocity: r->velocity_scale = a / 100.0; break;
        case nt::ArticulationAction::ScaleLength: r->length_scale = a / 100.0; break;
        case nt::ArticulationAction::ScaleVelocityAndLength:
            r->velocity_scale = a / 100.0;
            r->length_scale = b / 100.0;
            break;
    }
    health_.setText(health(), juce::dontSendNotification);
}

void ArticulationMapPanel::learn(int row)
{
    if (learning_ == row) {
        context_.engine.cancel_learn();
        learning_ = -1;
    } else {
        learning_ = row;
        context_.engine.arm_learn();
        status_.setText("Listening: press the keyswitch, move the controller or send the program on your MIDI keyboard",
                        juce::dontSendNotification);
    }
    for (int r = 0; r < static_cast<int>(rows_.size()); ++r) {
        const nt::ArticulationRule* rule = r == 0 ? (map_.has_natural ? &map_.natural : nullptr) : map_.rule_for(kRows[r - 1]);
        rows_[static_cast<std::size_t>(r)]->show(rule);
    }
}

void ArticulationMapPanel::timerCallback()
{
    if (learning_ < 0) return;
    const auto got = context_.engine.take_learned();
    if (!got) return;
    const int row = learning_;
    learning_ = -1;
    const std::uint8_t status = (*got)[0] & 0xF0;
    juce::String said;
    if (status == 0x90) {
        set_action(row, 1);
        set_values(row, (*got)[1], std::max<int>(1, (*got)[2]));
        said = "keyswitch " + juce::MidiMessage::getMidiNoteName((*got)[1], true, true, 3);
    } else if (status == 0xB0) {
        set_action(row, 2);
        set_values(row, (*got)[1], (*got)[2]);
        said = "CC " + juce::String((*got)[1]) + " = " + juce::String((*got)[2]);
    } else if (status == 0xC0) {
        set_action(row, 3);
        set_values(row, (*got)[1], 0);
        said = "program " + juce::String((*got)[1]);
    }
    const nt::ArticulationRule* rule = row == 0 ? (map_.has_natural ? &map_.natural : nullptr) : map_.rule_for(kRows[row - 1]);
    rule_changed(row, rule);
    status_.setText("Learned " + said + " for " + row_name(row), juce::dontSendNotification);
}

juce::String ArticulationMapPanel::health() const
{
    const auto* t = context_.session.data().find_track(track_);
    if (!t) return {};
    project::Track probe = *t;
    probe.articulation_map = map_;
    return juce::String(project::map_health(probe).summary());
}

bool ArticulationMapPanel::apply()
{
    if (map_.name.empty()) map_.name = "Custom map";
    if (!map_.is_valid()) {
        status_.setText("The map has a value out of range", juce::dontSendNotification);
        return false;
    }
    const bool ok = context_.session.set_articulation_map(track_, map_);
    status_.setText(ok ? "Applied (Edit > Undo takes it back)" : "Not applied: the track is locked (approved)",
                    juce::dontSendNotification);
    health_.setText(health(), juce::dontSendNotification);
    return ok;
}

bool ArticulationMapPanel::save_to_patch()
{
    if (!apply()) return false;
    const auto* t = context_.session.data().find_track(track_);
    if (!t || !context_.library) return false;
    auto& lib = context_.library->lib();
    if (const auto* p = lib.find_patch(t->assignment.patch_id)) {
        auto copy = *p;
        copy.articulation_map = map_;
        copy.expression_map = map_.name;
        lib.update_patch(copy);
        context_.library->changed();
        status_.setText("Applied, and saved with the patch \"" + juce::String(p->display) + "\"", juce::dontSendNotification);
        return true;
    }
    status_.setText("Applied. The track's sound is not a saved patch yet: Library > Save sound first",
                    juce::dontSendNotification);
    return false;
}

void ArticulationMapPanel::paint(juce::Graphics& g) { g.fillAll(colours::panel); }

void ArticulationMapPanel::resized()
{
    auto b = getLocalBounds().reduced(12);
    title_.setBounds(b.removeFromTop(24));
    health_.setBounds(b.removeFromTop(20));
    health_.setColour(juce::Label::textColourId, health_.getText().startsWith("Healthy") ? colours::exact : colours::accent_warm);
    b.removeFromTop(6);
    auto top = b.removeFromTop(26);
    base_.setBounds(top.removeFromLeft(300));
    top.removeFromLeft(8);
    name_.setBounds(top);
    b.removeFromTop(6);
    auto dyn = b.removeFromTop(26);
    dynamics_.setBounds(dyn.removeFromLeft(240));
    dyn.removeFromLeft(8);
    dynamics_cc_.setBounds(dyn.removeFromLeft(140));
    b.removeFromTop(8);
    auto bottom = b.removeFromBottom(30);
    apply_.setBounds(bottom.removeFromRight(90));
    bottom.removeFromRight(6);
    save_patch_.setBounds(bottom.removeFromRight(180));
    bottom.removeFromRight(6);
    revert_.setBounds(bottom.removeFromRight(80));
    bottom.removeFromRight(8);
    status_.setBounds(bottom);
    b.removeFromBottom(6);
    view_.setBounds(b);
    const int w = std::max(200, b.getWidth() - view_.getScrollBarThickness());
    int y = 0;
    for (auto& r : rows_) {
        r->setBounds(0, y, w, 34);
        y += 34;
    }
    list_.setSize(w, std::max(y, 10));
}

void show_articulation_map(AppContext& context, const std::string& track_id)
{
    if (auto* w = map_window().getComponent()) delete w;  // one track at a time
    dialogs::show_tool(map_window(), "Articulation Map", 760, 640,
                       [&context, track_id] { return new ArticulationMapPanel(context, track_id); });
}

ArticulationMapPanel* articulation_map_panel() { return map_slot(); }

// --- Missing instruments ---------------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> missing_instruments(AppContext& context)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& t : context.session.data().tracks) {
        if (t.instrument_plugin.empty()) continue;
        if (context.plugins.known().getTypeForIdentifierString(juce::String(t.instrument_plugin.identifier)) == nullptr)
            out.emplace_back(t.name + " - " + t.instrument_plugin.name, t.id);
    }
    return out;
}

namespace {
class MissingPanel final : public juce::Component {
public:
    explicit MissingPanel(AppContext& context) : context_(context)
    {
        setComponentID("missing_instruments");
        missing_slot() = this;
        intro_.setText("These plug-ins are not installed on this computer. The built-in placeholder plays the parts "
                       "until you choose substitutes; the plug-in states stay in the project for when they are back.",
                       juce::dontSendNotification);
        intro_.setFont(juce::FontOptions(12.0f));
        intro_.setColour(juce::Label::textColourId, colours::text);
        intro_.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(intro_);
        for (const auto& [label, id] : missing_instruments(context_)) {
            auto l = std::make_unique<juce::Label>();
            l->setText(label, juce::dontSendNotification);
            l->setColour(juce::Label::textColourId, colours::text);
            auto b = std::make_unique<juce::TextButton>("Find substitute");
            b->setTitle("Find a substitute for " + juce::String(label));
            const std::string track = id;
            b->onClick = [this, track] {
                context_.session.select_track(track);
                if (context_.show_library_for_track) context_.show_library_for_track();
            };
            addAndMakeVisible(*l);
            addAndMakeVisible(*b);
            labels_.push_back(std::move(l));
            buttons_.push_back(std::move(b));
        }
    }
    ~MissingPanel() override
    {
        if (missing_slot() == this) missing_slot() = nullptr;
    }
    void paint(juce::Graphics& g) override { g.fillAll(colours::panel); }
    void resized() override
    {
        auto b = getLocalBounds().reduced(12);
        intro_.setBounds(b.removeFromTop(44));
        b.removeFromTop(6);
        for (std::size_t i = 0; i < labels_.size(); ++i) {
            auto row = b.removeFromTop(28);
            buttons_[i]->setBounds(row.removeFromRight(130));
            labels_[i]->setBounds(row);
            b.removeFromTop(4);
        }
    }

private:
    AppContext& context_;
    juce::Label intro_;
    std::vector<std::unique_ptr<juce::Label>> labels_;
    std::vector<std::unique_ptr<juce::TextButton>> buttons_;
};
}  // namespace

int show_missing_instruments(AppContext& context)
{
    const int n = static_cast<int>(missing_instruments(context).size());
    if (n == 0) return 0;
    if (auto* w = missing_window().getComponent()) delete w;
    dialogs::show_tool(missing_window(), "Missing Instruments", 560, 140 + 32 * std::min(n, 12),
                       [&context] { return new MissingPanel(context); });
    return n;
}

juce::Component* missing_instruments_panel() { return missing_slot(); }

}  // namespace vcc::desktop
