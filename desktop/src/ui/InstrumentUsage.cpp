#include "ui/InstrumentUsage.h"

#include <algorithm>
#include <map>
#include <set>

#include "system/ResourceMonitor.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "vcc/project/ArticulationMap.h"

namespace vcc::desktop {

namespace {

enum Column { colTrack = 1, colInstrument, colState, colMemory, colLoad };

juce::Component::SafePointer<dialogs::ToolWindow>& usage_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
InstrumentUsagePanel*& usage_slot()
{
    static InstrumentUsagePanel* p = nullptr;
    return p;
}

juce::String family_name(const std::string& family)
{
    if (family.empty()) return "Other";
    juce::String s(family);
    return s.substring(0, 1).toUpperCase() + s.substring(1);
}

juce::String join_names(const std::vector<juce::String>& names)
{
    if (names.empty()) return {};
    if (names.size() == 1) return names[0];
    juce::String s;
    for (std::size_t i = 0; i + 1 < names.size(); ++i) s += (i ? ", " : "") + names[i];
    return s + " and " + names.back();
}

}  // namespace

InstrumentUsagePanel::InstrumentUsagePanel(AppContext& context) : context_(context)
{
    usage_slot() = this;
    title_.setText("Instrument Usage", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(17.0f, juce::Font::bold));
    for (auto* l : {&title_, &summary_, &suggestion_, &progress_}) addAndMakeVisible(l);
    summary_.setColour(juce::Label::textColourId, colours::text_dim);
    suggestion_.setColour(juce::Label::textColourId, colours::accent_warm);
    suggestion_.setJustificationType(juce::Justification::topLeft);
    progress_.setColour(juce::Label::textColourId, colours::text);

    auto& h = table_.getHeader();
    const int sortable = juce::TableHeaderComponent::defaultFlags;
    h.addColumn("Track", colTrack, 170, 80, 400, sortable);
    h.addColumn("Instrument", colInstrument, 190, 80, 400, sortable);
    h.addColumn("State", colState, 130, 70, 220, sortable);
    h.addColumn("Memory", colMemory, 210, 80, 360, sortable);
    h.addColumn("Engine load", colLoad, 100, 70, 160, sortable);
    h.setSortColumnId(colMemory, false);
    table_.setModel(this);
    table_.setRowHeight(24);
    table_.setMultipleSelectionEnabled(true);
    table_.setColour(juce::ListBox::backgroundColourId, colours::panel);
    table_.setTitle("Instruments");
    addAndMakeVisible(table_);

    freeze_.setTooltip("Print the selected tracks and close their instruments, giving their memory back");
    suspend_.setTooltip("Stop processing the selected instruments, keeping them loaded (instant to resume)");
    reload_.setTooltip("Close the selected plug-ins and open them again from their saved settings");
    purge_.setTooltip("Purge (unload unused samples) is not offered by the VST3 plug-in format: Freeze & Unload gives the memory back");
    locate_.setTooltip("Select the track and show it in Producer");
    batch_.setTooltip("Freeze & Unload a group at once: the selection, a part's articulation tracks, or an instrument family");
    cancel_.setTooltip("Stop the freeze in progress (tracks already printed stay printed)");
    freeze_.onClick = [this] { context_.status(run(Action::FreezeUnload)); };
    suspend_.onClick = [this] { context_.status(run(Action::Suspend)); };
    reload_.onClick = [this] { context_.status(run(Action::Reload)); };
    purge_.onClick = [this] { context_.status(run(Action::Purge)); };
    locate_.onClick = [this] { context_.status(run(Action::Locate)); };
    batch_.onClick = [this] { show_batch_menu(); };
    cancel_.onClick = [this] { cancel_batch(); };
    for (auto* b : {&freeze_, &suspend_, &reload_, &purge_, &locate_, &batch_, &cancel_}) addAndMakeVisible(b);
    purge_.setEnabled(false);
    cancel_.setVisible(false);
    if (context_.resources) context_.resources->sample_now();
    refresh();
    startTimer(250);
}

InstrumentUsagePanel::~InstrumentUsagePanel()
{
    if (usage_slot() == this) usage_slot() = nullptr;
}

void InstrumentUsagePanel::refresh()
{
    const auto selected = selected_tracks();
    const auto& session = context_.session;
    std::map<std::string, PlaybackEngine::InstrumentUsage> usage;
    for (auto& u : context_.engine.instrument_usage()) usage[u.track_id] = u;
    ResourceMonitor::Snapshot snap;
    if (context_.resources) snap = context_.resources->latest();
    std::map<std::uintptr_t, const PluginHost::HostMemory*> hosts;
    for (const auto& h : snap.hosts) hosts[h.process] = &h;

    rows_.clear();
    int plugins = 0;
    std::set<std::uintptr_t> processes;
    double total_load = 0.0;
    for (const auto* t : session.music_tracks()) {
        if (t->kind != project::TrackKind::Instrument && t->kind != project::TrackKind::MidiOnly) continue;
        const auto it = usage.find(t->id);
        if (it == usage.end() && !t->articulations_expanded && !t->instrument_unloaded) continue;  // external MIDI
        Row r;
        r.track_id = t->id;
        r.track = juce::String(t->name);
        r.family = family_name(t->family);
        r.state = context_.track_state ? context_.track_state(t->id) : juce::String("LIVE");
        r.suspended = t->instrument_suspended;
        r.frozen = !t->print_signature.empty() && t->playback == "audio";
        if (it != usage.end()) {
            const auto& u = it->second;
            r.instrument = u.name;
            r.plugin = u.plugin;
            r.loaded = u.loaded;
            r.load = u.load_percent;
            r.load_measured = u.measured;
            if (u.plugin && u.loaded) {
                ++plugins;
                if (u.host_process) processes.insert(u.host_process);
            }
        } else {
            r.instrument = juce::String(t->instrument_plugin.empty() ? std::string("Unloaded") : t->instrument_plugin.name);
            r.loaded = false;
        }
        if (t->articulations_expanded) r.loaded = false;
        // Memory, as far as it is measured.
        if (!r.loaded) {
            r.memory = t->articulations_expanded ? "0 (its articulation tracks play)" : "0 (unloaded)";
            r.memory_bytes = 0;
            r.memory_exact = true;
        } else if (!r.plugin) {
            r.memory = "Built-in synth (in app)";
        } else if (const auto u = usage[t->id]; u.host_process == 0) {
            r.memory = "Not reported (in app)";
        } else if (auto h = hosts.find(u.host_process); h == hosts.end()) {
            r.memory = "Measuring...";
        } else if (h->second->bytes == 0) {
            r.memory = "Not reported";
        } else if (h->second->instances <= 1) {
            r.memory_bytes = h->second->bytes;
            r.memory_exact = true;
            r.memory = format_bytes(h->second->bytes);
        } else {
            // Shared: an even share, marked as an estimate, with the process's total.
            const int n = h->second->instances;
            r.memory_bytes = h->second->bytes / static_cast<std::uint64_t>(n);
            r.memory = "~" + format_bytes(*r.memory_bytes) + "  (1 of " + juce::String(n) + " in " + h->second->label + ", " +
                       format_bytes(h->second->bytes) + ")";
        }
        if (r.suspended) {
            r.load_measured = true;
            r.load = 0.0;
        }
        total_load += r.load_measured ? r.load : 0.0;
        rows_.push_back(std::move(r));
    }
    apply_sort();

    const double cpu = context_.engine.devices().getCpuUsage();
    const auto dot = juce::String::fromUTF8("  \xc2\xb7  ");
    summary_.setText(juce::String(static_cast<int>(rows_.size())) + " instruments" + dot + juce::String(plugins) + " plug-ins in " +
                         juce::String(static_cast<int>(processes.size())) + " host processes" + dot + "engine load " +
                         juce::String(juce::roundToInt(cpu * 100.0)) + "%" + dot + "VC Composer and its plug-ins " +
                         format_bytes(snap.total_bytes()) +
                         (snap.system.total > 0 ? dot + "computer " + format_bytes(snap.system.used()) + " / " +
                                                      format_bytes(snap.system.total)
                                                : juce::String()),
                     juce::dontSendNotification);

    // Gentle suggestions: what would help, never done without being asked.
    std::vector<const Row*> live;
    for (const auto& r : rows_)
        if (r.loaded && r.plugin && !r.frozen) live.push_back(&r);
    const double mem_used =
        snap.system.total > 0 ? static_cast<double>(snap.system.used()) / static_cast<double>(snap.system.total) : 0.0;
    juce::String tip;
    if (mem_used >= 0.8 && !live.empty()) {
        std::sort(live.begin(), live.end(), [](const Row* a, const Row* b) { return a->memory_bytes.value_or(0) > b->memory_bytes.value_or(0); });
        std::vector<juce::String> names;
        std::uint64_t freed = 0;
        for (std::size_t i = 0; i < live.size() && i < 3; ++i) {
            names.push_back(live[i]->track);
            freed += live[i]->memory_bytes.value_or(0);
        }
        tip = "Memory is high (" + juce::String(juce::roundToInt(mem_used * 100.0)) + "% of the computer's). Freeze & Unload " +
              join_names(names) + (freed > 0 ? " would give back about " + format_bytes(freed) : juce::String()) +
              ". Nothing is unloaded unless you choose to.";
    } else if (cpu >= 0.7 && !live.empty()) {
        std::sort(live.begin(), live.end(), [](const Row* a, const Row* b) { return a->load > b->load; });
        std::vector<juce::String> names;
        double share = 0.0;
        for (std::size_t i = 0; i < live.size() && i < 3; ++i) {
            names.push_back(live[i]->track);
            share += live[i]->load;
        }
        tip = "Engine load is high (" + juce::String(juce::roundToInt(cpu * 100.0)) + "%). Freezing " + join_names(names) + " (" +
              juce::String(share, 1) + "% between them) would lighten it; Suspend those you are not using. Nothing changes unless "
              "you choose to.";
    } else {
        tip = "No suggestions: memory and engine load are comfortable.";
    }
    suggestion_.setText(tip, juce::dontSendNotification);
    select_tracks(selected);
    table_.updateContent();
    table_.repaint();
    update_buttons();
}

void InstrumentUsagePanel::sort_by(Sort column, bool forwards)
{
    table_.getHeader().setSortColumnId(static_cast<int>(column), forwards);  // calls sortOrderChanged
}

void InstrumentUsagePanel::sortOrderChanged(int column, bool forwards)
{
    const auto selected = selected_tracks();
    sort_ = static_cast<Sort>(column);
    forwards_ = forwards;
    apply_sort();
    select_tracks(selected);
    table_.updateContent();
    table_.repaint();
}

void InstrumentUsagePanel::apply_sort()
{
    auto rank = [](const Row& r) {  // live first, then what is not using anything
        return r.state == "LIVE" ? 0 : r.state == "SUSPENDED" ? 1 : r.state.startsWith("STALE") ? 2 : r.state.startsWith("FROZEN") ? 3 : 4;
    };
    auto load = [](const Row& r) { return r.load_measured ? r.load : -1.0; };
    auto less = [&](const Row& a, const Row& b) {
        switch (sort_) {
            case Sort::Track: return a.track.compareNatural(b.track) < 0;
            case Sort::Instrument: return a.instrument.compareNatural(b.instrument) < 0;
            case Sort::State: return rank(a) < rank(b);
            case Sort::Memory: return a.memory_bytes.value_or(0) < b.memory_bytes.value_or(0);
            case Sort::Load: return load(a) < load(b);
        }
        return false;
    };
    // Stable: equal keys keep the project's track order.
    std::stable_sort(rows_.begin(), rows_.end(), [&](const Row& a, const Row& b) { return forwards_ ? less(a, b) : less(b, a); });
}

void InstrumentUsagePanel::timerCallback()
{
    const auto p = context_.freeze_progress ? context_.freeze_progress() : juce::String();
    progress_.setText(p.isEmpty() ? juce::String() : "Freezing: " + p, juce::dontSendNotification);
    if (cancel_.isVisible() != p.isNotEmpty()) {
        cancel_.setVisible(p.isNotEmpty());
        update_buttons();
    }
    if (++ticks_ % 4 == 0) refresh();
}

void InstrumentUsagePanel::select_tracks(const std::vector<std::string>& ids)
{
    juce::SparseSet<int> set;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (std::find(ids.begin(), ids.end(), rows_[i].track_id) != ids.end()) set.addRange({static_cast<int>(i), static_cast<int>(i) + 1});
    table_.setSelectedRows(set, juce::dontSendNotification);
    update_buttons();
}

std::vector<std::string> InstrumentUsagePanel::selected_tracks() const
{
    std::vector<std::string> out;
    const auto set = table_.getSelectedRows();
    for (int i = 0; i < set.size(); ++i)
        if (set[i] >= 0 && set[i] < static_cast<int>(rows_.size())) out.push_back(rows_[static_cast<std::size_t>(set[i])].track_id);
    return out;
}

void InstrumentUsagePanel::update_buttons()
{
    const auto ids = selected_tracks();
    bool any_live = false, all_suspended = !ids.empty(), any_plugin = false;
    for (const auto& r : rows_) {
        if (std::find(ids.begin(), ids.end(), r.track_id) == ids.end()) continue;
        any_live = any_live || (r.loaded && !(r.frozen && !r.loaded));
        all_suspended = all_suspended && r.suspended;
        any_plugin = any_plugin || (r.plugin && r.loaded);
    }
    const bool busy = context_.freeze_progress && context_.freeze_progress().isNotEmpty();
    freeze_.setEnabled(any_live && !busy);
    suspend_.setButtonText(all_suspended ? "Resume" : "Suspend");
    suspend_.setEnabled(!ids.empty());
    reload_.setEnabled(any_plugin);
    locate_.setEnabled(!ids.empty());
    batch_.setEnabled(!busy);
}

juce::String InstrumentUsagePanel::run(Action action)
{
    const auto ids = selected_tracks();
    if (ids.empty() && action != Action::Purge) return "Select one or more instruments first";
    auto& session = context_.session;
    switch (action) {
        case Action::FreezeUnload: {
            std::vector<std::string> targets;
            for (const auto& r : rows_)
                if (std::find(ids.begin(), ids.end(), r.track_id) != ids.end() && r.loaded) targets.push_back(r.track_id);
            return freeze_batch(targets);
        }
        case Action::Suspend: {
            bool all_suspended = true;
            for (const auto& id : ids)
                if (const auto* t = session.data().find_track(id); t && !t->instrument_suspended) all_suspended = false;
            int n = 0;
            for (const auto& id : ids) n += session.set_instrument_suspended(id, !all_suspended) ? 1 : 0;
            refresh();
            return (all_suspended ? "Resumed " : "Suspended ") + juce::String(n) + (n == 1 ? " instrument" : " instruments") +
                   (all_suspended ? "" : ": still loaded, not played; Resume brings them back at once");
        }
        case Action::Reload: {
            if (context_.capture_plugin_states) context_.capture_plugin_states();
            int n = 0;
            for (const auto& id : ids) n += context_.engine.reload_instrument(id) ? 1 : 0;
            refresh();
            return n == 0 ? juce::String("Nothing to reload: no plug-in selected")
                          : "Reloaded " + juce::String(n) + (n == 1 ? " plug-in" : " plug-ins") + " from their saved settings";
        }
        case Action::Purge:
            return "Purge is not offered by VST3 plug-ins; Freeze & Unload gives an instrument's memory back instead";
        case Action::Locate: {
            session.select_track(ids.front());
            context_.commands.invokeDirectly(cmd::showSequencer, true);
            const auto* t = session.data().find_track(ids.front());
            return t ? "Located " + juce::String(t->name) : juce::String();
        }
    }
    return {};
}

std::vector<std::pair<juce::String, std::vector<std::string>>> InstrumentUsagePanel::batch_targets() const
{
    std::vector<std::pair<juce::String, std::vector<std::string>>> out;
    const auto& d = context_.session.data();
    auto freezable = [&](const Row& r) { return r.loaded && !r.suspended; };
    auto label = [](const juce::String& what, std::size_t n) { return what + "  (" + juce::String(static_cast<int>(n)) + ")"; };
    // The selection.
    const auto ids = selected_tracks();
    std::vector<std::string> sel;
    for (const auto& r : rows_)
        if (std::find(ids.begin(), ids.end(), r.track_id) != ids.end() && freezable(r)) sel.push_back(r.track_id);
    if (!sel.empty()) out.push_back({label("Selected tracks", sel.size()), sel});
    // Articulation groups: a selected part's articulation tracks; every track of a sound.
    std::set<std::string> parts, sounds;
    for (const auto& id : ids)
        if (const auto* t = d.find_track(id)) {
            if (t->articulations_expanded) parts.insert(t->id);
            if (!t->articulation_parent.empty()) {
                parts.insert(t->articulation_parent);
                sounds.insert(t->articulation_group);
            }
        }
    for (const auto& p : parts) {
        std::vector<std::string> v;
        for (const auto& r : rows_)
            if (const auto* t = d.find_track(r.track_id); t && t->articulation_parent == p && freezable(r)) v.push_back(r.track_id);
        if (const auto* pt = d.find_track(p); pt && !v.empty())
            out.push_back({label("Articulation tracks of " + juce::String(pt->name), v.size()), v});
    }
    for (const auto& g : sounds) {
        std::vector<std::string> v;
        for (const auto& r : rows_)
            if (const auto* t = d.find_track(r.track_id); t && t->articulation_group == g && freezable(r)) v.push_back(r.track_id);
        if (!v.empty()) out.push_back({label("Every " + juce::String(project::articulation_group_name(g)) + " track", v.size()), v});
    }
    // Families.
    std::map<juce::String, std::vector<std::string>> families;
    for (const auto& r : rows_)
        if (freezable(r)) families[r.family].push_back(r.track_id);
    for (const auto& [f, v] : families) out.push_back({label("Family: " + f, v.size()), v});
    return out;
}

juce::String InstrumentUsagePanel::freeze_batch(const std::vector<std::string>& tracks)
{
    if (tracks.empty()) return "Nothing to freeze: those instruments are already unloaded";
    if (!context_.freeze_tracks) return {};
    if (context_.freeze_progress && context_.freeze_progress().isNotEmpty()) return "A freeze is already running";
    context_.freeze_tracks(tracks, true);
    update_buttons();
    return "Freezing and unloading " + juce::String(static_cast<int>(tracks.size())) +
           (tracks.size() == 1 ? " instrument" : " instruments") + "...";
}

void InstrumentUsagePanel::cancel_batch()
{
    if (context_.cancel_freeze) context_.cancel_freeze();
    context_.status("Freeze cancelled: tracks already printed stay printed, nothing was unloaded");
    update_buttons();
}

void InstrumentUsagePanel::show_batch_menu()
{
    juce::PopupMenu m;
    m.addSectionHeader("Freeze && Unload");
    for (const auto& [name, tracks] : batch_targets()) {
        const auto t = tracks;
        m.addItem(name, [this, t] { context_.status(freeze_batch(t)); });
    }
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&batch_));
}

void InstrumentUsagePanel::paint(juce::Graphics& g) { g.fillAll(colours::background); }

void InstrumentUsagePanel::resized()
{
    auto r = getLocalBounds().reduced(14);
    title_.setBounds(r.removeFromTop(26));
    summary_.setBounds(r.removeFromTop(22));
    r.removeFromTop(4);
    auto bottom = r.removeFromBottom(32);
    auto progress_row = r.removeFromBottom(30);
    suggestion_.setBounds(r.removeFromBottom(40));
    r.removeFromBottom(6);
    table_.setBounds(r);
    cancel_.setBounds(progress_row.removeFromRight(90).reduced(0, 3));
    progress_.setBounds(progress_row);
    for (auto* b : {&freeze_, &suspend_, &reload_, &purge_, &locate_}) {
        const int w = b == &freeze_ ? 132 : 84;
        b->setBounds(bottom.removeFromLeft(w).reduced(0, 2));
        bottom.removeFromLeft(6);
    }
    batch_.setBounds(bottom.removeFromRight(200).reduced(0, 2));
}

void InstrumentUsagePanel::paintRowBackground(juce::Graphics& g, int row, int width, int height, bool selected)
{
    g.fillAll(row % 2 == 0 ? colours::panel : colours::panel.brighter(0.045f));
    if (selected) {
        g.fillAll(colours::accent.withAlpha(0.22f));
        g.setColour(colours::accent);
        g.fillRect(0, 0, 3, height);
    }
    g.setColour(colours::outline.withAlpha(0.45f));
    g.fillRect(0, height - 1, width, 1);
}

void InstrumentUsagePanel::paintCell(juce::Graphics& g, int row, int column, int width, int height, bool)
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const Row& r = rows_[static_cast<std::size_t>(row)];
    juce::String text;
    juce::Colour c = colours::text;
    switch (column) {
        case colTrack: text = r.track; break;
        case colInstrument: text = r.instrument; c = r.plugin ? colours::text : colours::text_dim; break;
        case colState:
            text = r.state;
            c = r.state.startsWith("STALE") || r.state.startsWith("MISSING") ? colours::fallback
                : r.state == "LIVE"                                          ? colours::exact
                                                                             : colours::text_dim;
            break;
        case colMemory:
            text = r.memory;
            c = r.memory_bytes ? colours::text : colours::text_dim;
            break;
        case colLoad:
            text = r.suspended ? juce::String("0% (suspended)")
                   : !r.loaded ? juce::String("-")
                   : r.load_measured ? juce::String(r.load, r.load < 10.0 ? 1 : 0) + "%"
                                     : juce::String("Not playing yet");
            c = r.load >= 25.0 ? colours::fallback : r.load_measured ? colours::text : colours::text_dim;
            break;
        default: break;
    }
    g.setColour(c);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(text, juce::Rectangle<int>(0, 0, width, height).reduced(7, 0), juce::Justification::centredLeft, true);
}

void InstrumentUsagePanel::cellDoubleClicked(int row, int, const juce::MouseEvent&)
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    select_tracks({rows_[static_cast<std::size_t>(row)].track_id});
    context_.status(run(Action::Locate));
}

void show_instrument_usage(AppContext& context)
{
    dialogs::show_tool(usage_window(), "Instrument Usage", 860, 560, [&context] { return new InstrumentUsagePanel(context); });
}

InstrumentUsagePanel* instrument_usage_panel() { return usage_slot(); }

}  // namespace vcc::desktop
