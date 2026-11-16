#include "ui/console/MixTools.h"

#include "audio/PrintToAudio.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "ui/hw/HwTheme.h"

namespace vcc::desktop {

namespace {

juce::Component::SafePointer<dialogs::ToolWindow>& tools_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
MixToolsPanel*& tools_slot()
{
    static MixToolsPanel* p = nullptr;
    return p;
}

void heading(juce::Label& l, const juce::String& text)
{
    l.setText(text.toUpperCase(), juce::dontSendNotification);
    l.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    l.setColour(juce::Label::textColourId, colours::text_dim);
}

void fader(juce::Slider& s, const juce::String& title)
{
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    s.setRange(-60.0, 12.0, 0.1);
    s.setTextValueSuffix(" dB");
    s.setDoubleClickReturnValue(true, 0.0);
    s.setTitle(title);
}

/// The selected track's folder's tracks, or the track itself.
std::vector<std::string> members_for_selection(const session::Session& s)
{
    const auto* t = s.data().find_track(s.selected_track());
    if (!t) return {};
    std::vector<std::string> out;
    if (!t->folder_id.empty())
        for (const auto& x : s.data().tracks)
            if (x.folder_id == t->folder_id && x.kind != project::TrackKind::Folder) out.push_back(x.id);
    if (out.empty()) out.push_back(t->id);
    return out;
}

/// A checklist of tracks; `toggle` adds or removes one.
void show_members_menu(AppContext& c, const std::vector<std::string>& members, juce::Component* target,
                       std::function<void(const std::string&, bool)> toggle)
{
    juce::PopupMenu m;
    for (const auto& t : c.session.data().tracks) {
        if (t.kind == project::TrackKind::Folder) continue;
        const bool in = std::find(members.begin(), members.end(), t.id) != members.end();
        const std::string id = t.id;
        m.addItem(juce::String(t.name), true, in, [toggle, id, in] { toggle(id, !in); });
    }
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target));
}

void toggle_member(std::vector<std::string>& members, const std::string& id, bool on)
{
    members.erase(std::remove(members.begin(), members.end(), id), members.end());
    if (on) members.push_back(id);
}

}  // namespace

// --- Groups and VCAs ------------------------------------------------------------------------

class MixToolsPanel::GroupsTab final : public juce::Component {
public:
    explicit GroupsTab(AppContext& c) : context_(c)
    {
        heading(vca_head_, "VCA faders");
        heading(group_head_, "Mix groups");
        add_vca_.setTooltip("A VCA over the selected track's folder (or the track): change members with Members");
        add_group_.setTooltip("A group of the selected track's folder (or the track): faders, mutes and solos move together");
        for (auto* x : std::initializer_list<juce::Component*>{&vca_head_, &group_head_, &add_vca_, &add_group_, &hint_})
            addAndMakeVisible(*x);
        hint_.setFont(juce::FontOptions(11.0f));
        hint_.setColour(juce::Label::textColourId, colours::text_dim);
        hint_.setText("A VCA trims its members' levels without summing their audio. A group moves members' faders by "
                      "the same amount, and links mutes, solos and arms as ticked.",
                      juce::dontSendNotification);
    }
    juce::TextButton add_vca_{"+ VCA"}, add_group_{"+ Group"};

    void rebuild()
    {
        const auto& d = context_.session.data();
        std::string sig;
        for (const auto& v : d.vcas) sig += v.id + v.name + std::to_string(v.members.size()) + ";";
        sig += "|";
        for (const auto& g : d.mix_groups) sig += g.id + g.name + std::to_string(g.members.size()) + ";";
        if (sig != signature_) {
            signature_ = sig;
            rows_.clear();
            for (const auto& v : d.vcas) make_vca_row(v.id);
            for (const auto& g : d.mix_groups) make_group_row(g.id);
            resized();
        }
        refresh_values();
    }

    void refresh_values()
    {
        const auto& d = context_.session.data();
        for (auto& r : rows_) {
            if (r->vca) {
                for (const auto& v : d.vcas)
                    if (v.id == r->id) {
                        if (!r->level.isMouseButtonDown()) r->level.setValue(v.gain_db, juce::dontSendNotification);
                        r->toggles[0]->setToggleState(v.mute, juce::dontSendNotification);
                    }
            } else {
                for (const auto& g : d.mix_groups)
                    if (g.id == r->id) {
                        const bool flags[] = {g.active, g.link_gain, g.link_mute, g.link_solo, g.link_arm};
                        for (int i = 0; i < 5; ++i) r->toggles[static_cast<std::size_t>(i)]->setToggleState(flags[i], juce::dontSendNotification);
                    }
            }
        }
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(10);
        auto top = b.removeFromTop(22);
        add_vca_.setBounds(top.removeFromRight(80));
        vca_head_.setBounds(top);
        b.removeFromTop(4);
        for (auto& r : rows_)
            if (r->vca) r->setBounds(b.removeFromTop(30));
        b.removeFromTop(10);
        top = b.removeFromTop(22);
        add_group_.setBounds(top.removeFromRight(80));
        group_head_.setBounds(top);
        b.removeFromTop(4);
        for (auto& r : rows_)
            if (!r->vca) r->setBounds(b.removeFromTop(30));
        b.removeFromTop(8);
        hint_.setBounds(b.removeFromTop(34));
    }

private:
    struct Row final : public juce::Component {
        std::string id;
        bool vca = true;
        juce::Label name;
        juce::Slider level;
        std::vector<std::unique_ptr<juce::ToggleButton>> toggles;
        juce::TextButton members{"Members"}, remove{"x"};
        void resized() override
        {
            auto b = getLocalBounds().reduced(2);
            remove.setBounds(b.removeFromRight(28));
            b.removeFromRight(4);
            members.setBounds(b.removeFromRight(96));
            b.removeFromRight(6);
            name.setBounds(b.removeFromLeft(130));
            if (vca) {
                toggles[0]->setBounds(b.removeFromRight(64));
                b.removeFromRight(6);
                level.setBounds(b);
            } else {
                const int w = b.getWidth() / 5;
                for (auto& t : toggles) t->setBounds(b.removeFromLeft(w));
            }
        }
    };

    void make_vca_row(const std::string& id)
    {
        auto r = std::make_unique<Row>();
        r->id = id;
        r->vca = true;
        const auto* v = find_vca(id);
        r->name.setText(v ? juce::String(v->name) : juce::String(), juce::dontSendNotification);
        r->name.setEditable(false, true);
        r->name.setTitle("VCA name");
        r->name.onTextChange = [this, id, n = &r->name] {
            context_.session.update_vca(id, "Rename VCA", [&](project::Vca& x) { x.name = n->getText().toStdString(); });
        };
        fader(r->level, "VCA level");
        r->level.onValueChange = [this, id, s = &r->level] { context_.session.set_vca_gain(id, s->getValue()); };
        auto mute = std::make_unique<juce::ToggleButton>("Mute");
        mute->onClick = [this, id, m = mute.get()] {
            context_.session.update_vca(id, m->getToggleState() ? "Mute VCA" : "Unmute VCA",
                                        [&](project::Vca& x) { x.mute = m->getToggleState(); });
        };
        r->toggles.push_back(std::move(mute));
        r->members.setButtonText("Members (" + juce::String(v ? static_cast<int>(v->members.size()) : 0) + ")");
        r->members.setTitle("VCA members");
        r->members.onClick = [this, id, b = &r->members] {
            const auto* x = find_vca(id);
            if (!x) return;
            show_members_menu(context_, x->members, b, [this, id](const std::string& t, bool on) {
                context_.session.update_vca(id, "VCA members", [&](project::Vca& y) { toggle_member(y.members, t, on); });
            });
        };
        r->remove.setTitle("Remove VCA");
        r->remove.onClick = [this, id] { context_.session.remove_vca(id); };
        add_row(std::move(r));
    }

    void make_group_row(const std::string& id)
    {
        auto r = std::make_unique<Row>();
        r->id = id;
        r->vca = false;
        const project::MixGroup* g = nullptr;
        for (const auto& x : context_.session.data().mix_groups)
            if (x.id == id) g = &x;
        r->name.setText(g ? juce::String(g->name) : juce::String(), juce::dontSendNotification);
        r->name.setEditable(false, true);
        r->name.setTitle("Group name");
        r->name.onTextChange = [this, id, n = &r->name] {
            context_.session.update_mix_group(id, "Rename group", [&](project::MixGroup& x) { x.name = n->getText().toStdString(); });
        };
        const char* names[] = {"On", "Fader", "Mute", "Solo", "Arm"};
        for (int i = 0; i < 5; ++i) {
            auto t = std::make_unique<juce::ToggleButton>(names[i]);
            t->setTitle(juce::String("Group ") + names[i]);
            t->onClick = [this, id, i, b = t.get()] {
                const bool on = b->getToggleState();
                context_.session.update_mix_group(id, "Group links", [&](project::MixGroup& x) {
                    bool* f[] = {&x.active, &x.link_gain, &x.link_mute, &x.link_solo, &x.link_arm};
                    *f[i] = on;
                });
            };
            r->toggles.push_back(std::move(t));
        }
        r->members.setButtonText("Members (" + juce::String(g ? static_cast<int>(g->members.size()) : 0) + ")");
        r->members.setTitle("Group members");
        r->members.onClick = [this, id, b = &r->members] {
            for (const auto& x : context_.session.data().mix_groups)
                if (x.id == id)
                    show_members_menu(context_, x.members, b, [this, id](const std::string& t, bool on) {
                        context_.session.update_mix_group(id, "Group members",
                                                          [&](project::MixGroup& y) { toggle_member(y.members, t, on); });
                    });
        };
        r->remove.setTitle("Remove group");
        r->remove.onClick = [this, id] { context_.session.remove_mix_group(id); };
        add_row(std::move(r));
    }

    void add_row(std::unique_ptr<Row> r)
    {
        for (auto* c : std::initializer_list<juce::Component*>{&r->name, &r->members, &r->remove}) r->addAndMakeVisible(*c);
        if (r->vca) r->addAndMakeVisible(r->level);
        for (auto& t : r->toggles) r->addAndMakeVisible(*t);
        addAndMakeVisible(*r);
        rows_.push_back(std::move(r));
    }

    const project::Vca* find_vca(const std::string& id) const
    {
        for (const auto& v : context_.session.data().vcas)
            if (v.id == id) return &v;
        return nullptr;
    }

    AppContext& context_;
    juce::Label vca_head_, group_head_, hint_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::string signature_;
};

// --- Monitor and cue mixes -----------------------------------------------------------------

class MixToolsPanel::MonitorTab final : public juce::Component {
public:
    explicit MonitorTab(AppContext& c) : context_(c)
    {
        heading(monitor_head_, "Monitor (what the speakers hear; exports never do)");
        heading(cue_head_, "Cue mixes (headphones: pre-fader sends to their own outputs)");
        dim_.setTitle("Dim");
        mono_.setTitle("Mono");
        mute_.setTitle("Monitor mute");
        dim_db_.setTitle("Dim amount");
        dim_db_.setSliderStyle(juce::Slider::LinearHorizontal);
        dim_db_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
        dim_db_.setRange(-40.0, -6.0, 1.0);
        dim_db_.setTextValueSuffix(" dB");
        speakers_.setTitle("Speaker outputs");
        for (int i = 0; i < 4; ++i) speakers_.addItem("Speakers on outputs " + juce::String(2 * i + 1) + "-" + juce::String(2 * i + 2), i + 1);
        auto apply = [this] { push(); };
        dim_.onClick = apply;
        mono_.onClick = apply;
        mute_.onClick = apply;
        dim_db_.onValueChange = apply;
        speakers_.onChange = apply;
        cue_box_.setTitle("Cue mix");
        cue_box_.onChange = [this] { rebuild_cue(); };
        cue_out_.setTitle("Cue outputs");
        for (int i = 1; i <= 3; ++i) cue_out_.addItem("Outputs " + juce::String(2 * i + 1) + "-" + juce::String(2 * i + 2), i);
        cue_out_.onChange = [this] {
            const auto id = cue_id();
            if (!id.empty())
                context_.session.update_bus(id, "Cue outputs", [&](project::Bus& b) {
                    b.output = "hw:" + std::to_string(cue_out_.getSelectedId());
                });
        };
        hint_.setFont(juce::FontOptions(11.0f));
        hint_.setColour(juce::Label::textColourId, colours::text_dim);
        hint_.setText("Enable the extra outputs in Audio and MIDI Settings to hear cue mixes and other speaker pairs.",
                      juce::dontSendNotification);
        view_.setViewedComponent(&sends_, false);
        view_.setScrollBarsShown(true, false);
        for (auto* x : std::initializer_list<juce::Component*>{&monitor_head_, &dim_, &dim_db_, &mono_, &mute_, &speakers_,
                                                              &cue_head_, &cue_box_, &cue_out_, &view_, &hint_})
            addAndMakeVisible(*x);
        load();
    }
    juce::TextButton add_cue_{"+ Cue mix"};

    void load()
    {
        const auto m = context_.engine.monitor();
        dim_.setToggleState(m.dim, juce::dontSendNotification);
        dim_db_.setValue(m.dim_db, juce::dontSendNotification);
        mono_.setToggleState(m.mono, juce::dontSendNotification);
        mute_.setToggleState(m.mute, juce::dontSendNotification);
        speakers_.setSelectedId(m.pair + 1, juce::dontSendNotification);
    }

    void push()
    {
        PlaybackEngine::Monitor m;
        m.dim = dim_.getToggleState();
        m.dim_db = dim_db_.getValue();
        m.mono = mono_.getToggleState();
        m.mute = mute_.getToggleState();
        m.pair = std::max(0, speakers_.getSelectedId() - 1);
        context_.engine.set_monitor(m);
        auto& ui = hw::ui_settings();
        ui.setValue("monitor_dim", m.dim);
        ui.setValue("monitor_dim_db", m.dim_db);
        ui.setValue("monitor_mono", m.mono);
        ui.setValue("monitor_mute", m.mute);
        ui.setValue("monitor_pair", m.pair);
        ui.saveIfNeeded();
    }

    std::string cue_id() const
    {
        const int i = cue_box_.getSelectedId() - 1;
        return i >= 0 && i < static_cast<int>(cue_ids_.size()) ? cue_ids_[static_cast<std::size_t>(i)] : std::string();
    }

    void rebuild()
    {
        const auto& d = context_.session.data();
        std::vector<std::string> ids;
        for (const auto& b : d.buses)
            if (b.kind == "cue") ids.push_back(b.id);
        std::string sig;
        for (const auto& id : ids) sig += id + d.find_bus(id)->name + ";";
        for (const auto& t : d.tracks) sig += t.id + ",";
        if (sig != signature_) {
            signature_ = sig;
            const auto keep = cue_id();
            cue_ids_ = ids;
            cue_box_.clear(juce::dontSendNotification);
            for (std::size_t i = 0; i < ids.size(); ++i) cue_box_.addItem(juce::String(d.find_bus(ids[i])->name), static_cast<int>(i) + 1);
            int sel = ids.empty() ? 0 : 1;
            for (std::size_t i = 0; i < ids.size(); ++i)
                if (ids[i] == keep) sel = static_cast<int>(i) + 1;
            cue_box_.setSelectedId(sel, juce::dontSendNotification);
            rebuild_cue();
        } else {
            refresh_levels();
        }
    }

    void rebuild_cue()
    {
        rows_.clear();
        const auto id = cue_id();
        const auto* bus = context_.session.data().find_bus(id);
        cue_out_.setEnabled(bus != nullptr);
        if (bus) {
            const auto hw = juce::String(bus->output);
            cue_out_.setSelectedId(hw.startsWith("hw:") ? juce::jlimit(1, 3, hw.substring(3).getIntValue()) : 1,
                                   juce::dontSendNotification);
            for (const auto& t : context_.session.data().tracks) {
                if (t.kind == project::TrackKind::Folder) continue;
                auto row = std::make_unique<SendRow>();
                row->track = t.id;
                row->name.setText(juce::String(t.name), juce::dontSendNotification);
                row->name.setColour(juce::Label::textColourId, colours::text);
                fader(row->level, "Cue send for " + juce::String(t.name));
                row->level.setRange(-61.0, 12.0, 0.5);
                row->level.setTextValueSuffix({});
                row->level.textFromValueFunction = [](double v) {
                    return v <= -60.5 ? juce::String("off") : juce::String(v, 1) + " dB";
                };
                row->level.onValueChange = [this, r = row.get(), id] { set_send(r->track, id, r->level.getValue()); };
                row->addAndMakeVisible(row->name);
                row->addAndMakeVisible(row->level);
                sends_.addAndMakeVisible(*row);
                rows_.push_back(std::move(row));
            }
        }
        refresh_levels();
        resized();
    }

    void refresh_levels()
    {
        const auto id = cue_id();
        for (auto& r : rows_) {
            if (r->level.isMouseButtonDown()) continue;
            double level = -61.0;
            if (const auto* t = context_.session.data().find_track(r->track))
                for (const auto& s : t->sends)
                    if (s.bus_id == id && !s.mute) level = s.level_db;
            r->level.setValue(level, juce::dontSendNotification);
        }
    }

    void set_send(const std::string& track, const std::string& cue, double db)
    {
        const auto* t = context_.session.data().find_track(track);
        if (!t || cue.empty()) return;
        int index = -1;
        for (std::size_t i = 0; i < t->sends.size(); ++i)
            if (t->sends[i].bus_id == cue) index = static_cast<int>(i);
        if (index < 0) {
            if (db > -60.5) context_.session.add_send(track, cue, true, db);
            return;
        }
        context_.session.update_send(track, index, "Cue send", [&](project::Send& s) {
            s.level_db = std::max(-60.0, db);
            s.mute = db <= -60.5;
            s.pre_fader = true;
        });
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(10);
        monitor_head_.setBounds(b.removeFromTop(20));
        auto r = b.removeFromTop(26);
        dim_.setBounds(r.removeFromLeft(70));
        dim_db_.setBounds(r.removeFromLeft(220));
        r.removeFromLeft(10);
        mono_.setBounds(r.removeFromLeft(70));
        mute_.setBounds(r.removeFromLeft(70));
        r.removeFromLeft(10);
        speakers_.setBounds(r);
        b.removeFromTop(12);
        r = b.removeFromTop(22);
        add_cue_.setBounds(r.removeFromRight(100));
        cue_head_.setBounds(r);
        b.removeFromTop(4);
        r = b.removeFromTop(26);
        cue_box_.setBounds(r.removeFromLeft(220));
        r.removeFromLeft(8);
        cue_out_.setBounds(r.removeFromLeft(160));
        hint_.setBounds(b.removeFromBottom(30));
        b.removeFromTop(6);
        view_.setBounds(b);
        const int w = std::max(200, b.getWidth() - view_.getScrollBarThickness());
        int y = 0;
        for (auto& row : rows_) {
            row->setBounds(0, y, w, 28);
            y += 28;
        }
        sends_.setSize(w, std::max(10, y));
    }

private:
    struct SendRow final : public juce::Component {
        std::string track;
        juce::Label name;
        juce::Slider level;
        void resized() override
        {
            auto b = getLocalBounds().reduced(2);
            name.setBounds(b.removeFromLeft(160));
            level.setBounds(b);
        }
    };

    AppContext& context_;
    juce::Label monitor_head_, cue_head_, hint_;
    juce::ToggleButton dim_{"Dim"}, mono_{"Mono"}, mute_{"Mute"};
    juce::Slider dim_db_;
    juce::ComboBox speakers_, cue_box_, cue_out_;
    juce::Viewport view_;
    juce::Component sends_;
    std::vector<std::unique_ptr<SendRow>> rows_;
    std::vector<std::string> cue_ids_;
    std::string signature_;
};

// --- Panel -----------------------------------------------------------------------------------

MixToolsPanel::MixToolsPanel(AppContext& context) : context_(context)
{
    setComponentID("mix_tools");
    tools_slot() = this;
    groups_ = std::make_unique<GroupsTab>(context_);
    monitor_ = std::make_unique<MonitorTab>(context_);
    groups_->add_vca_.onClick = [this] { add_vca(); };
    groups_->add_group_.onClick = [this] { add_group(); };
    monitor_->add_cue_.onClick = [this] { add_cue(); };
    groups_->addAndMakeVisible(groups_->add_vca_);
    groups_->addAndMakeVisible(groups_->add_group_);
    monitor_->addAndMakeVisible(monitor_->add_cue_);
    tabs_.addTab("Groups and VCAs", colours::panel, groups_.get(), false);
    tabs_.addTab("Monitor and cue mixes", colours::panel, monitor_.get(), false);
    addAndMakeVisible(tabs_);
    rebuild();
    // Polled, not a session listener: tool windows outlive the main window
    // at shutdown, and the rows only rebuild when their shape changes.
    startTimerHz(8);
}

MixToolsPanel::~MixToolsPanel()
{
    if (tools_slot() == this) tools_slot() = nullptr;
}

void MixToolsPanel::timerCallback()
{
    if (isShowing()) rebuild();
}

void MixToolsPanel::rebuild()
{
    groups_->rebuild();
    monitor_->rebuild();
}

std::string MixToolsPanel::add_vca()
{
    const auto members = members_for_selection(context_.session);
    const auto* t = context_.session.data().find_track(context_.session.selected_track());
    const auto* folder = t ? context_.session.data().find_track(t->folder_id) : nullptr;
    const auto id = context_.session.add_vca(folder ? folder->name : t ? t->name : std::string(), members);
    rebuild();
    return id;
}

std::string MixToolsPanel::add_group()
{
    const auto members = members_for_selection(context_.session);
    const auto* t = context_.session.data().find_track(context_.session.selected_track());
    const auto* folder = t ? context_.session.data().find_track(t->folder_id) : nullptr;
    const auto id = context_.session.add_mix_group(folder ? folder->name : t ? t->name : std::string(), members);
    rebuild();
    return id;
}

std::string MixToolsPanel::add_cue()
{
    const auto id = context_.session.add_bus("Cue " + std::to_string(1 + std::count_if(
                                                              context_.session.data().buses.begin(),
                                                              context_.session.data().buses.end(),
                                                              [](const project::Bus& b) { return b.kind == "cue"; })),
                                             "cue");
    rebuild();
    return id;
}

void MixToolsPanel::show_tab(int index) { tabs_.setCurrentTabIndex(index); }

void MixToolsPanel::set_monitor(bool dim, bool mono, bool mute, int pair)
{
    PlaybackEngine::Monitor m = context_.engine.monitor();
    m.dim = dim;
    m.mono = mono;
    m.mute = mute;
    m.pair = pair;
    context_.engine.set_monitor(m);
    monitor_->load();
    monitor_->push();
}

void MixToolsPanel::paint(juce::Graphics& g) { g.fillAll(colours::panel); }
void MixToolsPanel::resized() { tabs_.setBounds(getLocalBounds()); }

void show_mix_tools(AppContext& context, int tab)
{
    dialogs::show_tool(tools_window(), "Groups, VCAs and Monitor", 760, 560, [&context] { return new MixToolsPanel(context); });
    if (auto* p = tools_slot()) p->show_tab(tab);
}

MixToolsPanel* mix_tools_panel() { return tools_slot(); }

void restore_monitor(AppContext& context)
{
    auto& ui = hw::ui_settings();
    PlaybackEngine::Monitor m;
    m.dim = ui.getBoolValue("monitor_dim", false);
    m.dim_db = ui.getDoubleValue("monitor_dim_db", -20.0);
    m.mono = ui.getBoolValue("monitor_mono", false);
    m.mute = false;  // never start silent
    m.pair = ui.getIntValue("monitor_pair", 0);
    context.engine.set_monitor(m);
}

juce::String bounce_in_place(AppContext& context, const std::string& track_id)
{
    const auto* t = context.session.data().find_track(track_id);
    if (!t) return "Select the track to bounce";
    if (t->kind == project::TrackKind::Folder || t->kind == project::TrackKind::Aux)
        return "Bounce in place works on audio, instrument and MIDI tracks";
    PrintRequest rq;
    rq.tracks = {track_id};
    rq.include_fx = true;
    rq.play_audio = true;
    const auto& sel = context.session.selection();
    if (sel.has_range()) {
        rq.start = sel.from;
        rq.end = sel.to;
        rq.whole_range = true;
    } else {
        rq.start = 0;
        rq.end = std::max<Ticks>(context.session.content_end(), kTicksPerWhole);
    }
    context.engine.stop();
    const auto items = print_to_audio(context, rq);
    if (items.empty() || !items.front().ok)
        return "Not bounced: " + (items.empty() ? juce::String("nothing to render") : items.front().message);
    return "Bounced " + juce::String(t->name) + " in place (" + items.front().message +
           "); the clips it replaced are on a take lane - Edit > Undo puts them back";
}

}  // namespace vcc::desktop
