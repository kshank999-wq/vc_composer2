#include "ui/TrackPanel.h"

#include "audio/PlaceholderSynth.h"
#include "ui/AudioTrackStyle.h"
#include "ui/Theme.h"

namespace vcc::desktop {

namespace {

juce::Colour status_colour(const project::Track& t, const juce::String& engine_status)
{
    if (engine_status.startsWith("Missing")) return colours::missing;
    switch (t.assignment.status) {
        case project::ResolutionStatus::Exact: return colours::exact;
        case project::ResolutionStatus::Fallback: return colours::fallback;
        default: return colours::placeholder;
    }
}

bool recordable(const project::Track& t)
{
    return t.kind == project::TrackKind::Audio || t.kind == project::TrackKind::Reference;
}

/// Record-arm button: a red dot, lit when the track is armed.
class ArmButton final : public juce::Button {
public:
    ArmButton() : juce::Button("Arm") {}
    void paintButton(juce::Graphics& g, bool highlighted, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        const juce::Colour red{0xffe23b2e};
        const bool on = getToggleState();
        g.setColour(on ? red.withAlpha(0.25f) : (down ? colours::panel_raised.brighter(0.1f) : colours::panel_raised));
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(on ? red : (highlighted ? colours::outline.brighter(0.3f) : colours::outline));
        g.drawRoundedRectangle(r, 3.0f, 1.0f);
        const float d = std::min(r.getWidth(), r.getHeight()) * 0.5f;
        g.setColour(on ? red : red.withAlpha(highlighted ? 0.55f : 0.35f));
        g.fillEllipse(r.withSizeKeepingCentre(d, d));
    }
};

/// One track row: colour, name, version, M/S, status; recordable tracks add
/// record arm, input, monitoring and an input meter (Main Spec §11).
class TrackRow final : public juce::Component, private juce::Timer {
public:
    static constexpr int kHeight = 52;
    static constexpr int kRecordableHeight = 78;

    TrackRow(AppContext& c, std::string id) : context_(c), id_(std::move(id))
    {
        addChildComponent(arm_);
        addChildComponent(input_);
        addChildComponent(monitor_);
        arm_.setClickingTogglesState(false);
        arm_.setTooltip("Record arm: the next recording pass goes onto this track");
        arm_.setComponentID("arm_" + juce::String(id_));
        arm_.onClick = [this] {
            const auto* t = context_.session.data().find_track(id_);
            if (t) context_.session.set_record_armed(id_, !t->record_armed);
        };
        input_.setTooltip("Input: the device channel(s) this track records");
        input_.setComponentID("input_" + juce::String(id_));
        input_.onChange = [this] { input_chosen(input_.getSelectedId()); };
        monitor_.setTooltip("Monitoring: Auto hears the input while armed (stopped or recording); Input always; Off never");
        monitor_.setComponentID("monitor_" + juce::String(id_));
        monitor_.addItem("Auto", 1);
        monitor_.addItem("Input", 2);
        monitor_.addItem("Off", 3);
        monitor_.onChange = [this] {
            const int sel = monitor_.getSelectedId();
            const std::string mode = sel == 2 ? "input" : sel == 3 ? "off" : "auto";
            context_.session.update_track_mix(id_, "Monitor", [mode](project::Track& t) { t.monitor = mode; });
        };
        addAndMakeVisible(version_);
        addAndMakeVisible(mute_);
        addAndMakeVisible(solo_);
        mute_.setClickingTogglesState(true);
        solo_.setClickingTogglesState(true);
        mute_.setColour(juce::TextButton::buttonOnColourId, colours::mute);
        solo_.setColour(juce::TextButton::buttonOnColourId, colours::solo);
        mute_.setTooltip("Mute");
        solo_.setTooltip("Solo");
        mute_.onClick = [this] { context_.session.set_mute(id_, mute_.getToggleState()); };
        solo_.onClick = [this] { context_.session.set_solo(id_, solo_.getToggleState()); };
        version_.setTooltip("Track version (Main Spec 15): switch, or duplicate to try an alternative");
        version_.onChange = [this] {
            const int sel = version_.getSelectedId();
            if (sel == kDuplicate) {
                context_.session.duplicate_version(id_);
                return;
            }
            const auto* t = context_.session.data().find_track(id_);
            if (t && sel > 0 && sel <= static_cast<int>(t->versions.size()))
                context_.session.set_active_version(id_, t->versions[static_cast<std::size_t>(sel - 1)].id);
        };
        refresh();
    }

    void refresh()
    {
        const auto* t = context_.session.data().find_track(id_);
        if (!t) return;
        mute_.setToggleState(t->mute, juce::dontSendNotification);
        solo_.setToggleState(t->solo, juce::dontSendNotification);
        version_.clear(juce::dontSendNotification);
        int active = 0;
        for (std::size_t i = 0; i < t->versions.size(); ++i) {
            version_.addItem(t->versions[i].name, static_cast<int>(i) + 1);
            if (t->versions[i].id == t->active_version_id) active = static_cast<int>(i) + 1;
        }
        version_.addSeparator();
        version_.addItem("Duplicate as new version", kDuplicate);
        version_.setSelectedId(active, juce::dontSendNotification);
        version_.setVisible(t->kind != project::TrackKind::Folder && !t->versions.empty());
        mute_.setVisible(t->kind != project::TrackKind::Folder);
        solo_.setVisible(t->kind != project::TrackKind::Folder);

        const bool rec = recordable(*t);
        for (juce::Component* comp : std::initializer_list<juce::Component*>{&arm_, &input_, &monitor_})
            comp->setVisible(rec);
        if (rec) {
            arm_.setToggleState(t->record_armed, juce::dontSendNotification);
            fill_inputs(*t);
            monitor_.setSelectedId(t->monitor == "input" ? 2 : t->monitor == "off" ? 3 : 1, juce::dontSendNotification);
        }
        if (rec && t->record_armed) {
            if (!isTimerRunning()) startTimerHz(24);
        } else {
            stopTimer();
            level_[0] = level_[1] = 0.0f;
        }
        resized();
        repaint();
    }

    int preferred_height() const
    {
        const auto* t = context_.session.data().find_track(id_);
        if (!t) return kHeight;
        if (t->kind == project::TrackKind::Folder) return 24;
        return recordable(*t) ? kRecordableHeight : kHeight;
    }

    void paint(juce::Graphics& g) override
    {
        const auto* t = context_.session.data().find_track(id_);
        if (!t) return;
        const bool selected = context_.session.selected_track() == id_;
        const bool folder = t->kind == project::TrackKind::Folder;
        auto r = getLocalBounds();
        g.setColour(selected ? colours::selection.withAlpha(0.35f) : (folder ? colours::background : colours::panel));
        g.fillRect(r);
        g.setColour(t->kind == project::TrackKind::Audio ? audio_track_colour(*t) : colour_from_hex(t->color));
        g.fillRect(r.removeFromLeft(folder ? 3 : 5));
        g.setColour(colours::outline);
        g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
        auto text = getLocalBounds().withTrimmedLeft(folder ? 10 : 22).withTrimmedRight(8);
        if (folder) {
            g.setColour(colours::text_dim);
            g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
            g.drawText(juce::String(t->name).toUpperCase(), text, juce::Justification::centredLeft);
            return;
        }
        const juce::String status = context_.engine.instrument_status(id_);
        g.setColour(status_colour(*t, status));
        g.fillEllipse(10.0f, 10.0f, 8.0f, 8.0f);
        g.setColour(t->kind == project::TrackKind::Reference ? colours::accent_warm : colours::text);
        g.setFont(juce::FontOptions(14.0f));
        g.drawText(t->name, text.removeFromTop(24).withTrimmedTop(4), juce::Justification::centredLeft, true);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(11.0f));
        juce::String sub = t->kind == project::TrackKind::Reference ? juce::String("Temp - not exported")
                                                                    : status;
        const bool rec = recordable(*t);
        if (t->kind == project::TrackKind::Audio) sub = t->input_stereo ? "Stereo" : "Mono";
        g.drawText(sub, text.withHeight(kHeight - 24).withTrimmedRight(rec ? 172 : 150).withTrimmedBottom(4),
                   juce::Justification::centredLeft, true);

        // Input meter of an armed track (what the next pass will record).
        if (rec && !meter_area_.isEmpty()) {
            auto m = meter_area_.toFloat();
            g.setColour(colours::background);
            g.fillRoundedRectangle(m, 2.0f);
            const int channels = t->input_stereo ? 2 : 1;
            const float bar_h = (m.getHeight() - 3.0f) / static_cast<float>(channels);
            for (int ch = 0; ch < channels; ++ch) {
                // -60..0 dBFS across the meter.
                const float db = juce::Decibels::gainToDecibels(level_[ch], -60.0f);
                const float frac = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
                auto bar = juce::Rectangle<float>(m.getX() + 1.0f, m.getY() + 1.0f + static_cast<float>(ch) * (bar_h + 1.0f),
                                                  (m.getWidth() - 2.0f) * frac, bar_h);
                g.setColour(level_[ch] >= 1.0f ? colours::missing : db > -6.0f ? colours::fallback : colours::exact);
                g.fillRect(bar);
            }
            g.setColour(colours::outline);
            g.drawRoundedRectangle(m, 2.0f, 1.0f);
        }
    }

    void resized() override
    {
        const auto* t = context_.session.data().find_track(id_);
        const bool rec = t && recordable(*t);
        auto r = getLocalBounds().withTrimmedTop(24).withHeight(kHeight - 24).reduced(6, 3);
        if (rec) {
            arm_.setBounds(r.removeFromRight(22));
            r.removeFromRight(3);
            auto line = getLocalBounds().withTrimmedTop(kHeight - 2).withTrimmedBottom(5).withTrimmedLeft(22).withTrimmedRight(6);
            meter_area_ = line.removeFromRight(46).reduced(0, 5);
            line.removeFromRight(6);
            monitor_.setBounds(line.removeFromRight(66));
            line.removeFromRight(4);
            input_.setBounds(line);
        } else {
            meter_area_ = {};
        }
        solo_.setBounds(r.removeFromRight(22));
        r.removeFromRight(3);
        mute_.setBounds(r.removeFromRight(22));
        r.removeFromRight(6);
        version_.setBounds(r.removeFromRight(92));
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        context_.session.select_track(id_);
        if (e.mods.isPopupMenu()) TrackPanel::show_track_menu(context_, id_);
    }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        const auto* t = context_.session.data().find_track(id_);
        if (!t || t->kind == project::TrackKind::Folder) return;
        auto* editor = new juce::TextEditor();
        editor->setText(t->name);
        editor->setBounds(22, 3, getWidth() - 30, 22);
        editor->onReturnKey = [this, editor] {
            context_.session.rename_track(id_, editor->getText().toStdString());
            juce::MessageManager::callAsync([editor] { delete editor; });
        };
        editor->onFocusLost = editor->onEscapeKey = [editor] { juce::MessageManager::callAsync([editor] { delete editor; }); };
        addAndMakeVisible(editor);
        editor->grabKeyboardFocus();
        editor->selectAll();
    }

    const std::string& id() const noexcept { return id_; }

private:
    static constexpr int kNoInput = 1;
    static constexpr int kFirstInput = 100;
    static constexpr int kMono = 50, kStereo = 51;

    void fill_inputs(const project::Track& t)
    {
        input_.clear(juce::dontSendNotification);
        const int n = context_.engine.num_inputs();
        input_.addItem("No input", kNoInput);
        const int step = t.input_stereo ? 2 : 1;
        for (int ch = 0; ch + (step - 1) < n; ch += step) {
            juce::String name = context_.engine.input_name(ch);
            if (t.input_stereo) name << " + " << context_.engine.input_name(ch + 1);
            input_.addItem(name, kFirstInput + ch);
        }
        input_.addSeparator();
        input_.addItem("Mono", kMono);
        input_.addItem("Stereo", kStereo);
        const bool valid = t.input >= 0 && t.input + (step - 1) < n;
        input_.setSelectedId(valid ? kFirstInput + t.input : kNoInput, juce::dontSendNotification);
        input_.setItemEnabled(kMono, t.input_stereo);
        input_.setItemEnabled(kStereo, !t.input_stereo);
        if (n == 0) input_.setTooltip("No audio input device is open - choose one in Tools > Audio and MIDI Settings");
    }

    void input_chosen(int id)
    {
        if (id == kMono || id == kStereo) {
            const bool stereo = id == kStereo;
            context_.session.update_track_mix(id_, "Input format", [stereo](project::Track& t) { t.input_stereo = stereo; });
            return;
        }
        const int channel = id >= kFirstInput ? id - kFirstInput : -1;
        context_.session.update_track_mix(id_, "Input", [channel](project::Track& t) { t.input = channel; });
    }

    void timerCallback() override
    {
        const auto* t = context_.session.data().find_track(id_);
        if (!t || !t->record_armed || t->input < 0) {
            level_[0] = level_[1] = 0.0f;
        } else {
            for (int ch = 0; ch < (t->input_stereo ? 2 : 1); ++ch) {
                const float peak = context_.engine.take_input_peak(t->input + ch);
                level_[ch] = std::max(peak, level_[ch] * 0.85f);  // fall back smoothly
            }
        }
        repaint(meter_area_);
    }

    static constexpr int kDuplicate = 9999;
    AppContext& context_;
    std::string id_;
    juce::ComboBox version_;
    juce::TextButton mute_{"M"}, solo_{"S"};
    ArmButton arm_;
    juce::ComboBox input_, monitor_;
    juce::Rectangle<int> meter_area_;
    float level_[2] = {0.0f, 0.0f};
};

}  // namespace

// --- Track list -------------------------------------------------------------------------

class TrackPanel::TrackList final : public juce::Component, private juce::Timer {
public:
    explicit TrackList(AppContext& c) : context_(c)
    {
        addAndMakeVisible(viewport_);
        viewport_.setViewedComponent(&content_, false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(add_);
        addAndMakeVisible(add_template_);
        add_.onClick = [this] { TrackPanel::show_add_track_menu(context_, &add_); };
        add_template_.onClick = [this] { context_.commands.invokeDirectly(cmd::addTemplate, true); };
        listener_ = context_.session.add_listener([this](session::Change change) {
            if (change == session::Change::Document) rebuild();
            if (change == session::Change::Selection)
                for (auto& r : rows_) r->repaint();
        });
        rebuild();
        startTimer(1000);  // instrument status can change when plug-ins load
    }
    ~TrackList() override { context_.session.remove_listener(listener_); }

    void rebuild()
    {
        // Rows are rebuilt only when the track list itself changed; otherwise refreshed.
        std::vector<std::string> ids;
        for (const auto& t : context_.session.data().tracks)
            if (t.kind != project::TrackKind::Video) ids.push_back(t.id);
        bool same = ids.size() == rows_.size();
        for (std::size_t i = 0; same && i < ids.size(); ++i) same = rows_[i]->id() == ids[i];
        if (!same) {
            rows_.clear();
            for (const auto& id : ids) {
                rows_.push_back(std::make_unique<TrackRow>(context_, id));
                content_.addAndMakeVisible(*rows_.back());
            }
        } else {
            for (auto& r : rows_) r->refresh();
        }
        layout_rows();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto buttons = r.removeFromBottom(34).reduced(6, 4);
        add_.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2 - 3));
        buttons.removeFromLeft(6);
        add_template_.setBounds(buttons);
        viewport_.setBounds(r);
        layout_rows();
    }

private:
    void timerCallback() override
    {
        for (auto& r : rows_) r->repaint();
    }

    void layout_rows()
    {
        int y = 0;
        const int w = viewport_.getMaximumVisibleWidth();
        for (auto& r : rows_) {
            const int h = r->preferred_height();
            r->setBounds(0, y, w, h);
            y += h;
        }
        content_.setSize(w, std::max(y, viewport_.getHeight()));
    }

    AppContext& context_;
    int listener_ = 0;
    juce::Viewport viewport_;
    juce::Component content_;
    std::vector<std::unique_ptr<TrackRow>> rows_;
    juce::TextButton add_{"+ Track"}, add_template_{"+ Template"};
};

// --- Library -------------------------------------------------------------------------------

class TrackPanel::Library final : public juce::Component, private juce::ListBoxModel {
public:
    explicit Library(AppContext& c) : context_(c)
    {
        addAndMakeVisible(search_);
        search_.setTextToShowWhenEmpty("Search instruments, families, regions...", colours::text_dim);
        search_.onTextChange = [this] { filter(); };
        addAndMakeVisible(list_);
        list_.setModel(this);
        list_.setRowHeight(36);
        addAndMakeVisible(audition_);
        addAndMakeVisible(add_);
        addAndMakeVisible(plugins_);
        audition_.onClick = [this] { audition(); };
        add_.onClick = [this] { add(); };
        plugins_.onClick = [this] { context_.commands.invokeDirectly(cmd::scanPlugins, true); };
        filter();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(6);
        search_.setBounds(r.removeFromTop(26));
        r.removeFromTop(6);
        auto buttons = r.removeFromBottom(28);
        r.removeFromBottom(6);
        list_.setBounds(r);
        const int w = (buttons.getWidth() - 12) / 3;
        audition_.setBounds(buttons.removeFromLeft(w));
        buttons.removeFromLeft(6);
        add_.setBounds(buttons.removeFromLeft(w));
        buttons.removeFromLeft(6);
        plugins_.setBounds(buttons);
    }

private:
    int getNumRows() override { return static_cast<int>(shown_.size()); }

    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= getNumRows()) return;
        const auto* def = shown_[static_cast<std::size_t>(row)];
        g.fillAll(selected ? colours::selection : colours::panel);
        g.setColour(colour_from_hex(templates::family_color(def->family)));
        g.fillRect(0, 0, 4, h);
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(def->name, 10, 2, w - 14, h / 2, juce::Justification::bottomLeft, true);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(11.0f));
        juce::String sub = templates::family_display(def->family);
        if (!def->region.empty()) sub << "  -  " << def->region;
        g.drawText(sub, 10, h / 2, w - 14, h / 2 - 2, juce::Justification::topLeft, true);
    }

    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { add(); }

    void filter()
    {
        shown_.clear();
        const auto q = search_.getText().toLowerCase();
        for (const auto& i : context_.session.catalogs().instruments.all()) {
            juce::String hay = juce::String(i.name + " " + i.family + " " + i.region + " " + i.id).toLowerCase();
            for (const auto& t : i.tags) hay << " " << juce::String(t).toLowerCase();
            if (q.isEmpty() || hay.contains(q)) shown_.push_back(&i);
        }
        list_.updateContent();
        list_.repaint();
    }

    const templates::InstrumentDef* chosen() const
    {
        const int row = list_.getSelectedRow();
        return row >= 0 && row < static_cast<int>(shown_.size()) ? shown_[static_cast<std::size_t>(row)] : nullptr;
    }

    void audition()
    {
        const auto* def = chosen();
        if (!def) return;
        // A short phrase in the instrument's comfortable middle register.
        std::vector<NoteEvent> notes;
        if (def->unpitched) {
            const int kit[] = {36, 42, 38, 42, 36, 36, 38, 46};
            for (int i = 0; i < 8; ++i) notes.push_back({i * kTicksPerQuarter / 2, kTicksPerQuarter / 2, kit[i], 100, 9, {}});
        } else {
            const int mid = (def->range_low + def->range_high) / 2;
            const int base = mid - (mid % 12);
            const int steps[] = {0, 4, 7, 12, 7, 4, 0};
            for (int i = 0; i < 7; ++i)
                notes.push_back({i * kTicksPerQuarter / 2, kTicksPerQuarter / 2, base + steps[i], 90, 0, {}});
            notes.back().duration = kTicksPerQuarter * 2;
        }
        context_.engine.preview(notes, def->family, def->unpitched ? -1 : def->gm_program, 110.0, false);
        context_.status("Auditioning " + juce::String(def->name) + " on the built-in placeholder sound");
    }

    void add()
    {
        if (const auto* def = chosen()) context_.session.add_track(def->name, def->id);
    }

    AppContext& context_;
    juce::TextEditor search_;
    juce::ListBox list_;
    juce::TextButton audition_{"Audition"}, add_{"Add track"}, plugins_{"Plug-ins..."};
    std::vector<const templates::InstrumentDef*> shown_;
};

// --- Panel ------------------------------------------------------------------------------------

TrackPanel::TrackPanel(AppContext& c) : context_(c)
{
    tracks_ = std::make_unique<TrackList>(c);
    library_ = std::make_unique<Library>(c);
    tabs_.addTab("Tracks", colours::panel, tracks_.get(), false);
    tabs_.addTab("Library", colours::panel, library_.get(), false);
    tabs_.setTabBarDepth(28);
    tabs_.setOutline(0);
    addAndMakeVisible(tabs_);
}

TrackPanel::~TrackPanel() = default;

void TrackPanel::paint(juce::Graphics& g) { g.fillAll(colours::panel); }

void TrackPanel::resized() { tabs_.setBounds(getLocalBounds()); }

void TrackPanel::show_track_menu(AppContext& c, const std::string& id)
{
    const auto* t = c.session.data().find_track(id);
    if (!t || t->kind == project::TrackKind::Folder) return;
    juce::PopupMenu m;
    m.addSectionHeader(t->name);
    m.addItem("Duplicate as new version", [&c, id] { c.session.duplicate_version(id); });
    juce::PopupMenu instruments;
    instruments.addItem("Built-in placeholder sound", true, t->instrument_plugin.empty(),
                        [&c, id] { c.session.set_instrument_plugin(id, {}); });
    instruments.addSeparator();
    auto list = c.plugins.instruments();
    if (list.isEmpty()) instruments.addItem("No instrument plug-ins scanned yet", false, false, [] {});
    for (const auto& d : list) {
        auto ref = c.plugins.ref_for(d);
        const bool current = ref.identifier == t->instrument_plugin.identifier;
        instruments.addItem(d.manufacturerName + " - " + d.name, true, current,
                            [&c, id, ref] { c.session.set_instrument_plugin(id, ref); });
    }
    instruments.addSeparator();
    instruments.addItem("Scan for plug-ins...", [&c] { c.commands.invokeDirectly(cmd::scanPlugins, true); });
    m.addSubMenu("Instrument", instruments);
    m.addItem("Open instrument window", !t->instrument_plugin.empty(),
              false, [&c, id] { if (c.show_plugin_window) c.show_plugin_window(id); });
    m.addSeparator();
    m.addItem("Include in deliverable export", true, t->include_in_export,
              [&c, id, inc = t->include_in_export] { c.session.set_include_in_export(id, !inc); });
    m.addItem(juce::String("Status: ") + t->assignment.message, false, false, [] {});
    m.addSeparator();
    if (recordable(*t)) {
        m.addItem("Record arm", true, t->record_armed,
                  [&c, id, armed = t->record_armed] { c.session.set_record_armed(id, !armed); });
        m.addItem("Import audio onto this track...", [&c, id] {
            c.session.select_track(id);
            c.commands.invokeDirectly(cmd::importAudio, true);
        });
    }
    m.addSeparator();
    m.addItem("Remove track", [&c, id] { c.session.remove_track(id); });
    m.showMenuAsync({});
}

void TrackPanel::show_add_track_menu(AppContext& c, juce::Component* target)
{
    juce::PopupMenu m;
    m.addItem("Audio track (mono)", [&c] { c.session.add_audio_track("Audio", false); });
    m.addItem("Audio track (stereo)", [&c] { c.session.add_audio_track("Audio", true); });
    m.addItem("Temp / reference track", [&c] { c.session.add_reference_track("Temp Score"); });
    std::map<std::string, juce::PopupMenu> by_family;
    for (const auto& i : c.session.catalogs().instruments.all()) {
        const std::string id = i.id, name = i.name;
        by_family[i.family].addItem(name, [&c, id, name] { c.session.add_track(name, id); });
    }
    m.addSeparator();
    for (const auto& f : templates::family_order())
        if (by_family.count(f)) m.addSubMenu(templates::family_display(f), by_family[f]);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target));
}

}  // namespace vcc::desktop
