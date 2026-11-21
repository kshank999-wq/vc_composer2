#include "ui/TrackPanel.h"

#include <map>

#include "ui/library/ArticulationMapWindow.h"
#include "vcc/project/Percussion.h"
#include "ui/library/LibraryBrowser.h"

#include "ui/Deferred.h"
#include "ui/composer/TransposePopover.h"

#include <algorithm>

#include "audio/PlaceholderSynth.h"
#include "audio/SoundPack.h"
#include "ui/AudioTrackStyle.h"
#include "ui/SequencerView.h"
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
        input_.setTitle("Input");
        input_.setTooltip("Input: the device channel(s) this track records");
        input_.setComponentID("input_" + juce::String(id_));
        input_.onChange = [this] { input_chosen(input_.getSelectedId()); };
        monitor_.setTitle("Monitoring");
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
        version_.setTitle("Track version");
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
            version_.addItem(juce::String(t->versions[i].name) + (t->versions[i].approved ? juce::String(" (approved)") : juce::String()),
                             static_cast<int>(i) + 1);
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
            if (change == session::Change::Document) deferred_.request();
            if (change == session::Change::Selection)
                for (auto& r : rows_) r->repaint();  // a stale row repaints harmlessly; the rebuild follows
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
    Deferred deferred_{[this] { rebuild(); }};
    juce::Viewport viewport_;
    juce::Component content_;
    std::vector<std::unique_ptr<TrackRow>> rows_;
    juce::TextButton add_{"+ Track"}, add_template_{"+ Template"};
};

// --- Panel ------------------------------------------------------------------------------------

TrackPanel::TrackPanel(AppContext& c) : context_(c)
{
    tracks_ = std::make_unique<TrackList>(c);
    library_ = std::make_unique<LibraryBrowser>(c);
    tabs_.addTab("Tracks", colours::panel, tracks_.get(), false);
    tabs_.setTabBarDepth(28);
    tabs_.setOutline(0);
    addAndMakeVisible(tabs_);
}

TrackPanel::~TrackPanel() = default;

void TrackPanel::paint(juce::Graphics& g) { g.fillAll(colours::panel); }

void TrackPanel::resized() { tabs_.setBounds(getLocalBounds()); }

juce::PopupMenu TrackPanel::instrument_menu(AppContext& c, const std::string& id)
{
    juce::PopupMenu instruments;
    const auto* t = c.session.data().find_track(id);
    if (!t) return instruments;
    instruments.addItem("Sound Setup...  (your libraries: open the player, load, name, save)", [&c, id] {
        c.session.select_track(id);
        c.commands.invokeDirectly(cmd::soundSetup, true);
    });
    instruments.addSeparator();
    instruments.addItem(sounds::state() == sounds::State::Ready ? "Built-in sound (piano / General MIDI)" : "Built-in placeholder sound", true,
                        t->instrument_plugin.empty(), [&c, id] { c.session.set_instrument_plugin(id, {}); });
    instruments.addSeparator();
    // Virtual instruments by maker (EastWest, Spitfire, Native Instruments...).
    auto list = c.plugins.instruments();
    if (list.isEmpty()) instruments.addItem("No instrument plug-ins scanned yet", false, false, [] {});
    std::map<juce::String, juce::PopupMenu> by_maker;
    for (const auto& d : list) {
        auto ref = c.plugins.ref_for(d);
        const bool current = ref.identifier == t->instrument_plugin.identifier;
        const auto maker = d.manufacturerName.isEmpty() ? juce::String("Other") : d.manufacturerName;
        // Choosing one loads it and opens its window, as in any DAW.
        by_maker[maker].addItem(d.name + (d.pluginFormatName.isEmpty() ? juce::String() : "  (" + d.pluginFormatName + ")"), true,
                                current, [&c, id, ref, current] {
                                    if (!current) c.session.set_instrument_plugin(id, ref);
                                    if (c.show_plugin_window) c.show_plugin_window(id);
                                });
    }
    for (auto& [maker, menu] : by_maker) instruments.addSubMenu(maker, menu);
    instruments.addSeparator();
    instruments.addItem("Open instrument window", !t->instrument_plugin.empty(), false, [&c, id] {
        if (c.show_plugin_window) c.show_plugin_window(id);
    });
    // Every track at once, from the libraries here (EastWest, Spitfire...).
    {
        juce::PopupMenu from;
        from.addItem("Best match from all my libraries", [&c] {
            if (c.assign_from_libraries) c.assign_from_libraries({});
        });
        if (c.installed_libraries) {
            const auto libs = c.installed_libraries();
            if (!libs.empty()) from.addSeparator();
            for (const auto& [lib_id, name] : libs)
                from.addItem("Prefer " + juce::String(name), [&c, lib = lib_id] {
                    if (c.assign_from_libraries) c.assign_from_libraries(lib);
                });
        }
        instruments.addSubMenu("Assign sounds to all tracks from my libraries", from);
    }
    instruments.addItem("Library...", [&c, id] {
        c.session.select_track(id);
        if (c.show_library_for_track) c.show_library_for_track();
    });
    instruments.addItem("Scan for plug-ins...", [&c] { c.commands.invokeDirectly(cmd::scanPlugins, true); });
    return instruments;
}

void TrackPanel::open_instrument(AppContext& c, const std::string& id, juce::Component* target)
{
    const auto* t = c.session.data().find_track(id);
    if (!t) return;
    c.session.select_track(id);
    if (!t->instrument_plugin.empty() && c.show_plugin_window) {
        c.show_plugin_window(id);
        return;
    }
    auto options = juce::PopupMenu::Options();
    if (target) options = options.withTargetComponent(target);
    instrument_menu(c, id).showMenuAsync(options);
}

void TrackPanel::show_track_menu(AppContext& c, const std::string& id)
{
    const auto* t = c.session.data().find_track(id);
    if (!t || t->kind == project::TrackKind::Folder) return;
    juce::PopupMenu m;
    m.addSectionHeader(t->name);
    m.addItem("Clear Track...", [&c, id] {
        const auto* tr = c.session.data().find_track(id);
        if (!tr) return;
        const juce::String name(tr->name);
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Clear " + name + "?",
                                           "Are you sure you want to clear the track? Every note on its staff and its MIDI in "
                                           "Producer go (Undo brings them back).",
                                           "Clear Track", "Cancel", nullptr, juce::ModalCallbackFunction::create([&c, id, name](int r) {
                                               if (r == 1)
                                                   c.status(c.session.clear_track(id) ? name + " cleared (Undo brings it back)"
                                                                                      : name + " has nothing to clear");
                                           }));
    });
    m.addSeparator();
    m.addItem("Duplicate as new version", [&c, id] { c.session.duplicate_version(id); });
    if (const auto* v = t->active_version()) {
        const std::string vid = v->id;
        if (v->approved)
            m.addItem("Release approval of " + juce::String(v->name), [&c, id, vid] { c.session.set_version_approved(id, vid, false); });
        else
            m.addItem("Approve " + juce::String(v->name) + " (lock it)", [&c, id, vid] { c.session.set_version_approved(id, vid, true); });
    }
    juce::PopupMenu instruments = instrument_menu(c, id);
    m.addSubMenu("Instrument", instruments);
    m.addItem("Articulation map...", [&c, id] { show_articulation_map(c, id); });
    {
        juce::PopupMenu drums;
        drums.addItem("None (pitched)", true, t->percussion_map.empty(), [&c, id] { c.session.set_percussion_map(id, ""); });
        for (const auto& pm : project::builtin_percussion_maps()) {
            const std::string map_id = pm.id;
            drums.addItem(juce::String(pm.name), true, t->percussion_map == pm.id,
                          [&c, id, map_id] { c.session.set_percussion_map(id, map_id); });
        }
        if (const auto* pm = project::percussion_map_named(t->percussion_map)) {
            drums.addSeparator();
            for (const auto& sound : pm->sounds)
                drums.addItem(juce::String(sound.name) + "  -  " + juce::MidiMessage::getMidiNoteName(sound.written, true, true, 4) +
                                  (sound.notehead == 1 ? " (x)" : sound.notehead == 2 ? " (circled x)" : sound.notehead == 3 ? " (triangle)" : ""),
                              false, false, [] {});
        }
        m.addSubMenu("Percussion map", drums);
    }
    // Trans (Add-On spec §10): the popover, at the pointer.
    {
        std::string why;
        const bool can = c.session.can_transpose(*t, &why);
        const int s = t->transpose;
        m.addItem("Trans (transpose)" + juce::String(s != 0 ? "  " + juce::String(s > 0 ? "+" : "") + juce::String(s) + " st" : "") + "...",
                  can, false, [&c, id] {
                      const auto at = juce::Desktop::getMousePosition();
                      composer_ui::TransposePopover::show(c, id, juce::Rectangle<int>(at.x, at.y, 1, 1));
                  });
    }
    // Instrument range (Add-On spec §4).
    if (t->kind == project::TrackKind::Instrument || t->kind == project::TrackKind::MidiOnly) {
        using RP = session::Session::RangePolicy;
        const auto limits = c.session.range_limits(*t);
        auto name = [](int n) { return juce::MidiMessage::getMidiNoteName(n, true, true, 4); };
        juce::PopupMenu range;
        if (!limits.instrument.empty()) {
            range.addSectionHeader(juce::String(limits.instrument) + ": " + name(limits.playable_low) + "-" + name(limits.playable_high) +
                                   " sounding");
            range.addItem("Comfortable " + name(limits.comfortable_low) + "-" + name(limits.comfortable_high) +
                              (limits.has_extended ? juce::String() : juce::String("; no extended range")),
                          false, false, [] {});
        }
        auto policy = [&](const char* label, const std::string& value, bool on, bool enabled) {
            range.addItem(label, enabled, on, [&c, id, value] { c.session.set_range_policy(id, value); });
        };
        const bool def = limits.policy_is_default;
        policy("Default for the instrument", "", def, true);
        policy("Strict: refuse notes outside", "strict", !def && limits.policy == RP::Strict, !limits.instrument.empty());
        policy("Warn: allow, ringed", "warn", !def && limits.policy == RP::Warn, !limits.instrument.empty());
        policy("Extended range", "extended", !def && limits.policy == RP::Extended, limits.has_extended);
        policy("Off (synths, experiments)", "off", !def && limits.policy == RP::Off, true);
        range.addSeparator();
        const auto issues = c.session.range_issues(*t);
        range.addItem("Check range...", [&c, id] { show_range_report(c, id); });
        range.addItem("Move " + juce::String(static_cast<int>(issues.size())) + " notes into range", !issues.empty(), false,
                      [&c, id] {
                          const int moved = c.session.move_into_range(id);
                          c.status("Moved " + juce::String(moved) + " notes into range by octaves");
                      });
        m.addSubMenu("Range" + juce::String(issues.empty() ? "" : " (" + juce::String(static_cast<int>(issues.size())) + " outside)"),
                     range);
    }
    m.addSeparator();
    m.addItem("In the Score (and its parts)", true, t->include_in_export,
              [&c, id, inc = t->include_in_export] { c.session.set_in_score(id, !inc); });
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
    if (auto* seq = SequencerView::find(c)) {
        const bool shown = seq->automation_shown(id);
        m.addItem(shown ? "Hide Automation" : "Show Automation", [&c, id, shown] {
            if (auto* sv = SequencerView::find(c)) {
                sv->show_automation(id, !shown);
                if (!shown) c.commands.invokeDirectly(cmd::showSequencer, true);
            }
        });
        m.addItem("Hide All Automation", [&c] {
            if (auto* sv = SequencerView::find(c)) sv->hide_all_automation();
        });
        m.addSeparator();
    }
    m.addItem("Remove track", [&c, id] { c.session.remove_track(id); });
    m.showMenuAsync({});
}

juce::String TrackPanel::range_report(AppContext& c, const std::string& track_id)
{
    const auto& d = c.session.data();
    juce::String text;
    auto name = [](int n) { return juce::MidiMessage::getMidiNoteName(n, true, true, 4); };
    for (const auto& t : d.tracks) {
        if (!track_id.empty() && t.id != track_id) continue;
        const auto issues = c.session.range_issues(t);
        if (issues.empty()) continue;
        const auto limits = c.session.range_limits(t);
        text << t.name << " (" << name(limits.low) << "-" << name(limits.high) << ", "
             << session::Session::range_policy_name(limits.policy) << "): " << static_cast<int>(issues.size())
             << (issues.size() == 1 ? " note" : " notes") << "\n";
        int shown = 0;
        for (const auto& i : issues) {
            if (++shown > 8) {
                text << "   ...\n";
                break;
            }
            const int bar = d.meter.bar_at(i.tick);
            const auto sig = d.meter.at_bar(bar);
            const int beat = static_cast<int>((i.tick - d.meter.bar_start(bar)) / std::max<Ticks>(1, sig.beat_ticks())) + 1;
            text << "   bar " << bar << " beat " << beat << ": " << name(i.sounding) << (i.above ? " above" : " below") << "\n";
        }
    }
    return text.isEmpty() ? juce::String("Every note is inside its instrument's range.") : text.trimEnd();
}

juce::String TrackPanel::show_range_report(AppContext& c, const std::string& track_id)
{
    const auto text = range_report(c, track_id);
    auto* w = new juce::AlertWindow("Instrument ranges", "Notes outside their instrument's range (sounding pitch).",
                                    juce::MessageBoxIconType::NoIcon);
    w->setComponentID("range_report");
    w->addTextBlock(text);
    w->addButton("Move into range", 1);
    w->addButton("Close", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([&c, track_id](int r) {
        if (r != 1) return;
        std::vector<std::string> ids;
        for (const auto& t : c.session.data().tracks)
            if (track_id.empty() || t.id == track_id) ids.push_back(t.id);
        int moved = 0;
        for (const auto& id : ids) moved += c.session.move_into_range(id);
        c.status("Moved " + juce::String(moved) + " notes into range by octaves");
    }), true);
    return text;
}

juce::PopupMenu TrackPanel::sketch_area_menu(AppContext& c)
{
    // A piano first (the usual second sketch), then any instrument by family.
    juce::PopupMenu m;
    auto next_name = [&c](const std::string& base) {
        int n = 1;
        for (const auto* t : c.session.music_tracks())
            if (c.session.is_sketch(*t)) ++n;
        return base + " " + std::to_string(n);
    };
    m.addItem("Sketch Piano", [&c, next_name] { c.session.add_sketch_track(next_name("Sketch Piano"), "keys.piano"); });
    m.addSeparator();
    std::map<std::string, juce::PopupMenu> by_family;
    for (const auto& i : c.session.catalogs().instruments.all()) {
        const std::string id = i.id, name = i.name;
        by_family[i.family].addItem(name, [&c, id, name] { c.session.add_sketch_track("Sketch " + name, id); });
    }
    for (const auto& f : templates::family_order())
        if (by_family.count(f)) m.addSubMenu(templates::family_display(f), by_family[f]);
    return m;
}

void TrackPanel::show_add_track_menu(AppContext& c, juce::Component* target)
{
    juce::PopupMenu m;
    // The Sketch area: where ideas are worked out, outside the orchestra's groups.
    m.addSubMenu("Into the Sketch Area", sketch_area_menu(c));
    m.addSeparator();
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
