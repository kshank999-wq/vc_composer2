#include "ui/TrackPanel.h"

#include "audio/PlaceholderSynth.h"
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

/// One track row: colour, name, version, M/S, status.
class TrackRow final : public juce::Component {
public:
    TrackRow(AppContext& c, std::string id) : context_(c), id_(std::move(id))
    {
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
        repaint();
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
        g.setColour(colour_from_hex(t->color));
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
        g.drawText(sub, text.withTrimmedRight(150).withTrimmedBottom(4), juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedTop(24).reduced(6, 3);
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
    static constexpr int kDuplicate = 9999;
    AppContext& context_;
    std::string id_;
    juce::ComboBox version_;
    juce::TextButton mute_{"M"}, solo_{"S"};
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
            const auto* t = context_.session.data().find_track(r->id());
            const int h = t && t->kind == project::TrackKind::Folder ? 24 : 52;
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
    m.addItem("Remove track", [&c, id] { c.session.remove_track(id); });
    m.showMenuAsync({});
}

void TrackPanel::show_add_track_menu(AppContext& c, juce::Component* target)
{
    juce::PopupMenu m;
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
