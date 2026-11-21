#include "ui/Credits.h"
#include "ui/SoundsWindow.h"
#include "ui/MainComponent.h"

#include "ui/console/RackGear.h"
#include "ui/console/SectionZoom.h"
#include "ui/SteadyWindow.h"

#include "ui/composer/CircleOfFifths.h"
#include "ui/console/RoutingCanvas.h"
#include "ui/review/Review.h"
#include "ui/review/Relink.h"
#include "ui/EditTools.h"
#include "ui/NewProjectWizard.h"
#include "Preview.h"
#include "licensing/Account.h"
#include "licensing/AccountWindow.h"
#include "licensing/UpdateWindow.h"
#include "licensing/Updater.h"
#include "collab/Collaboration.h"
#include "collab/SharedWindow.h"
#include "ui/PictureTools.h"
#include "ui/PrintPreview.h"
#include "ui/InstrumentUsage.h"
#include "ui/ScoreTools.h"
#include "ui/library/SoundAssignments.h"
#include "ui/ShortcutEditor.h"
#include "ui/console/MixTools.h"
#include "ui/library/ArticulationMapWindow.h"
#include "ui/library/LibraryBrowser.h"
#include "vcc/theory/Chord.h"

#include "ui/hw/HwGallery.h"
#include "ui/hw/HwTheme.h"

#include <algorithm>
#include <juce_audio_utils/juce_audio_utils.h>

#include "ui/CueManager.h"
#include <cstring>

#include "ui/Dialogs.h"
#include "audio/ChannelDsp.h"
#include "system/PluginWatch.h"
#include "ui/StudioSetup.h"
#include "ui/RecentProjects.h"
#include "ui/TrackListPalette.h"
#include "ui/Import.h"
#include "ui/console/ConsoleView.h"
#include "ui/ScoreExchange.h"
#include "ui/StemExport.h"
#include "ui/VideoWindow.h"
#include "ui/WaveformCache.h"
#include "vcc/picture/Picture.h"
#include "ui/Theme.h"
#include "vcc/core/Files.h"
#include "vcc/engraving/PrintLayout.h"
#include "vcc/engraving/SvgRenderer.h"
#include "vcc/project/Automation.h"
#include "vcc/project/Render.h"

namespace vcc::desktop {

// --- Plug-in windows --------------------------------------------------------------------------

class MainComponent::PluginWindow final : public juce::DocumentWindow {
public:
    /// An instrument's window (`slot` < 0) or an effect insert's.
    PluginWindow(AppContext& c, std::string owner_id, int slot, juce::AudioPluginInstance& instance)
        : juce::DocumentWindow(instance.getName(), colours::background, juce::DocumentWindow::closeButton),
          context_(c), owner_id_(std::move(owner_id)), slot_(slot), instance_(instance),
          keepalive_(c.engine.plugin_keepalive(owner_id_, slot))
    {
        setUsingNativeTitleBar(true);
        juce::AudioProcessorEditor* editor = instance.hasEditor() ? instance.createEditorIfNeeded() : nullptr;
        if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor(instance);
        setContentOwned(editor, true);
        setResizable(editor->isResizable(), false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    ~PluginWindow() override
    {
        store();
        clearContentComponent();  // the editor goes while its plug-in is still here
    }

    /// The plug-in's settings go into the project (no undo step).
    void store()
    {
        const auto state = PluginHost::state_of(instance_);
        if (slot_ < 0) context_.session.store_plugin_state(owner_id_, state);
        else context_.session.store_insert_plugin_state(owner_id_, slot_, state);
    }

    void closeButtonPressed() override
    {
        store();
        setVisible(false);
    }

    juce::AudioPluginInstance* instance() noexcept { return &instance_; }
    const std::string& owner() const noexcept { return owner_id_; }
    int slot() const noexcept { return slot_; }

private:
    AppContext& context_;
    std::string owner_id_;
    int slot_;
    juce::AudioPluginInstance& instance_;
    std::shared_ptr<void> keepalive_;  ///< Declared last: released after the editor is gone.
};

// --- Main component -----------------------------------------------------------------------------

/// Window > Inspector's content: what is selected over the inspector. Its
/// place is remembered when it moves (to another screen, too).
class InspectorBox final : public juce::Component, private juce::ComponentListener {
public:
    static constexpr int kWidth = 268, kSelection = 190, kGap = 6;
    static int height() { return kSelection + kGap + composer_ui::Inspector::preferred_height(false) + 12; }
    InspectorBox(shell::SelectionPanel& selection, composer_ui::Inspector& inspector)
        : selection_(selection), inspector_(inspector)
    {
        setComponentID("inspector_box");
        addAndMakeVisible(selection_);
        addAndMakeVisible(inspector_);
        inspector_.set_horizontal(false);
        inspector_.set_compact(false);
        setSize(kWidth, height());
    }
    void paint(juce::Graphics& g) override { g.fillAll(hw::tok::chassis); }
    void resized() override
    {
        auto r = getLocalBounds().reduced(6);
        selection_.setBounds(r.removeFromTop(kSelection));
        r.removeFromTop(kGap);
        inspector_.setBounds(r.withHeight(std::min(r.getHeight(), composer_ui::Inspector::preferred_height(false))));
    }
    void parentHierarchyChanged() override
    {
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) {
            if (watched_ != w) {
                if (watched_ != nullptr) watched_->removeComponentListener(this);
                watched_ = w;
                w->addComponentListener(this);
            }
        }
    }
    ~InspectorBox() override
    {
        if (watched_ != nullptr) watched_->removeComponentListener(this);
    }

private:
    void componentMovedOrResized(juce::Component& c, bool, bool) override
    {
        const auto b = c.getBounds();
        hw::ui_settings().setValue("inspector_bounds", juce::String(b.getX()) + " " + juce::String(b.getY()) + " " +
                                                           juce::String(b.getWidth()) + " " + juce::String(b.getHeight()));
    }
    shell::SelectionPanel& selection_;
    composer_ui::Inspector& inspector_;
    juce::Component::SafePointer<juce::Component> watched_;
};

MainComponent::MainComponent(AppContext& context)
    : context_(context),
      deck_(context),
      toolbar_(context),
      edit_(context),
      markers_(context),
      new_track_(context),
      clips_(context),
      selection_(context),
      inspector_(context),
      channel_(context),
      print_(context),
      master_(context),
      status_strip_(context),
      keyboard_(context),
      mixer_deck_(context),
      page_view_(context),
      tracks_(context),
      composer_(context),
      sequencer_(context)
{
    // The UI theme (LED, LCD, staves) the user chose last time.
    hw::Theme::get().attach(&hw::ui_settings());
    // The Edit window (docs/spec/ui): top deck, toolbar, left rack, the
    // edit window around the current view, right panel, status strip.
    for (auto* c : std::initializer_list<juce::Component*>{&deck_, &toolbar_, &edit_, &markers_, &status_strip_, &keyboard_})
        addAndMakeVisible(c);
    // The selection readout and the inspector live in their own floating
    // window (Window > Inspector), not in a column beside the edit window.
    inspector_box_ = std::make_unique<InspectorBox>(selection_, inspector_);
    addChildComponent(new_track_);
    addChildComponent(clips_);
    addChildComponent(channel_);
    addChildComponent(print_);
    addChildComponent(master_);
    for (auto* k : {&channel_tab_, &print_tab_}) {
        addChildComponent(*k);
        k->set_led_dot(true);
        k->set_legend_size(10.5f);
        k->setWantsKeyboardFocus(false);
    }
    channel_tab_.onClick = [this] { show_right_tab(0); };
    print_tab_.onClick = [this] { show_right_tab(1); };
    addChildComponent(mixer_deck_);
    dock_key_.setComponentID("dock_toggle");
    dock_key_.setTooltip("Hide or show the bottom deck - the keyboard, or the mixer in Producer (Cmd/Ctrl+Alt+B)");
    dock_key_.onClick = [this] { set_dock_hidden(!dock_hidden_); };
    addAndMakeVisible(dock_key_);
    keyboard_.on_mixer = [this] { show_bottom_mixer(true); };
    mixer_deck_.on_keyboard = [this] { show_bottom_mixer(false); };
    mixer_deck_.on_full = [this](bool) { set_mixer_full(true); };
    mixer_deck_.on_console = [this] { show_console_window(); };
    mixer_deck_.on_pop_out = [this](bool out) { set_mixer_popped(out); };
    mixer_deck_.on_display = [this] {
        juce::PopupMenu m;
        for (const auto& [key, label] : display_keys())
            m.addItem(label, [this, key = key] {
                if (!deck_window_) return;
                for (const auto& d : juce::Desktop::getInstance().getDisplays().displays)
                    if (d.totalArea.toString() == key) deck_window_->setBounds(d.userArea);
            });
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&mixer_deck_.display_key()));
    };
    addAndMakeVisible(tracks_);
    addAndMakeVisible(composer_);
    addChildComponent(page_view_);
    toolbar_.on_page_view = [this](bool page) { set_page_view(page); };
    addChildComponent(sequencer_);
    deck_.on_view = [this](bool producer) { show_workspace(producer ? Workspace::Producer : Workspace::Composer); };
    edit_.on_stage = [this](int stage) { go_to_stage(stage); };
    toolbar_.on_zoom = [this](bool larger) {
        if (workspace_ == Workspace::Composer) {
            if (page_mode_) page_view_.zoom(larger);
            else composer_.zoom(larger);
        }
        else sequencer_.zoom(larger);
    };
    toolbar_.on_pencil = [this](bool on) { sequencer_.set_pencil_mode(on); };
    toolbar_.on_edit_mode = [this](int mode) {
        sequencer_.set_edit_mode(static_cast<SequencerView::EditMode>(mode));
        refresh_tool_keys();
    };
    toolbar_.on_smart = [this](bool on) {
        sequencer_.set_smart_tool(on);
        refresh_tool_keys();
    };
    // The Circle of Fifths' Send to Chord Mode: the palette set to the chord, opened.
    context_.send_to_chord_mode = [this](const std::string& symbol) {
        const auto chord = theory::parse_chord_symbol(symbol);
        if (!chord) return;
        auto& s = composer_.chord_state();
        s.letter = ((chord->root.letter % 7) + 7) % 7;
        s.accidental = std::clamp(chord->root.alteration, -1, 1);
        s.quality = chord->quality.id;
        s.alterations.clear();
        s.inversion = chord->bass ? 0 : chord->inversion;
        s.slash_bass = chord->bass ? chord->bass->name() : std::string();
        show_workspace(Workspace::Composer);
        composer_.set_chord_mode(true);
        composer_.chord_palette().refresh();
        refresh_tool_keys();
    };
    toolbar_.on_chord_mode = [this](bool on) {
        composer_.set_chord_mode(on);
        refresh_tool_keys();
        composer_.grabKeyboardFocus();
    };
    // The Composer keys and the inspector run notation commands on the
    // score being written, and hand the keyboard back to it.
    auto run = [this](composer::Command c) {
        composer_.perform(c);
        composer_.grabKeyboardFocus();
    };
    toolbar_.on_command = run;
    inspector_.on_command = run;
    // The keyboard deck: step input into the Composer view, which grows the
    // deck (and shrinks the edit window) as the keys zoom.
    keyboard_.on_command = run;
    keyboard_.cursor = [this] { return composer_.cursor(); };
    keyboard_.on_step = [this](int midi, bool chord) {
        // Step input writes in either mode, as typing does (Select governs clicks).
        if (workspace_ != Workspace::Composer) return false;
        const bool ok = composer_.enter_midi(midi, chord);
        composer_.grabKeyboardFocus();
        return ok;
    };
    keyboard_.on_height_changed = [this] { resized(); };
    // A header's Print key prints that track; Freeze prints it with effects.
    sequencer_.on_print_track = [this](const std::string& track, bool freeze) {
        show_right_tab(1);  // in a short window, the Print tab shows the print's progress
        print_.print_tracks({track}, freeze);
    };
    context_.freeze_tracks = [this](const std::vector<std::string>& tracks, bool unload) {
        if (tracks.empty() || print_.printing()) return;
        unload_after_print_ = unload ? std::set<std::string>(tracks.begin(), tracks.end()) : std::set<std::string>{};
        show_right_tab(1);
        print_.print_tracks(tracks, true, true, true);
    };
    print_.on_printed = [this](const std::vector<PrintItem>& items) {
        // Freeze & Unload: the prints are placed; now close the instruments,
        // their settings captured first so Unfreeze brings them back as they were.
        if (unload_after_print_.empty()) return;
        capture_plugin_states();
        int unloaded = 0;
        for (const auto& i : items)
            if (i.ok && unload_after_print_.count(i.track_id) && context_.session.set_instrument_unloaded(i.track_id, true)) ++unloaded;
        unload_after_print_.clear();
        if (unloaded > 0)
            context_.status("Frozen and unloaded " + juce::String(unloaded) + (unloaded == 1 ? " instrument" : " instruments") +
                            ": their memory is given back; Unfreeze reloads them");
    };
    context_.show_instrument_usage = [this] { show_instrument_usage(context_); };
    context_.go_to_start = [this] {
        context_.engine.locate(0);
        composer_.set_origin(0);
        sequencer_.scroll_to_start();
        page_view_.scroll_to_start();
    };
    collab::Controller::get().ask_conflicts = ask_conflicts;
    collab::Controller::get().attach(context_);
    // A little after start-up, quietly: is there a newer build on vc-composer.com?
    {
        juce::Component::SafePointer<MainComponent> self(this);
        // A newer build is fetched in the background - previews with no
        // account, releases once signed in or activated - and installed by
        // itself when nothing is unsaved or playing (or at the next start).
        auto idle = [self] {
            return self != nullptr && !self->context_.session.dirty() && !self->context_.engine.playing() &&
                   !self->context_.engine.recording();
        };
        auto ask = [self] { return self != nullptr && self->confirm_discard(); };
        set_after_sign_in([self, ask, idle] {
            if (self != nullptr) auto_update(self->context_, ask, idle);
        });
        // One downloaded last time goes in first, before any work starts.
        juce::Timer::callAfterDelay(2500, [self, idle] {
            if (self != nullptr) install_ready_update(self->context_, idle);
        });
        juce::Timer::callAfterDelay(20000, [self, ask, idle] {
            if (self == nullptr) return;
            if (Updater::may_download()) {
                auto_update(self->context_, ask, idle);
                return;
            }
            check_for_updates_quietly([self](juce::String version) {
                if (self == nullptr) return;
                self->update_waiting_ = version;
                self->context_.commands.commandStatusChanged();
                self->context_.status("VC Composer " + version + " is available: Help > Install Update");
            });
        });
    }
    context_.freeze_progress = [this]() -> juce::String {
        if (!print_.printing()) return {};
        return print_.progress_lcd().text().replace("\n", "  ");
    };
    context_.cancel_freeze = [this] {
        unload_after_print_.clear();
        print_.cancel();
    };
    context_.track_state = [this](const std::string& id) -> juce::String {
        const auto* t = context_.session.data().find_track(id);
        if (!t) return {};
        if (print_.printing_track(id)) return "RENDERING";
        // The signature hashes the track's music: worked out once per edit.
        const auto gen = context_.session.change_count();
        if (gen != freeze_cache_generation_) {
            freeze_cache_.clear();
            freeze_cache_generation_ = gen;
        }
        auto cached = freeze_cache_.find(id);
        if (cached == freeze_cache_.end()) cached = freeze_cache_.emplace(id, context_.session.freeze_state(id)).first;
        const auto state = cached->second;
        const juce::String unloaded = t->instrument_unloaded ? juce::String::fromUTF8(" \xc2\xb7 UNLOADED") : juce::String();
        if (state == project::FreezeState::Stale) return "STALE" + unloaded;
        if (state == project::FreezeState::Frozen) return "FROZEN" + unloaded;
        if (context_.engine.instrument_status(id).startsWith("Missing")) return "MISSING PLUG-IN";
        if (t->articulations_expanded) return "EXPANDED";
        if (t->instrument_suspended) return "SUSPENDED";
        return "LIVE";
    };

    layout_.setItemLayout(0, 220, 520, 300);
    layout_.setItemLayout(1, 5, 5, 5);
    layout_.setItemLayout(2, 300, -1.0, -1.0);
    divider_ = std::make_unique<juce::StretchableLayoutResizerBar>(&layout_, 1, true);
    addAndMakeVisible(*divider_);

    context_.status = [this](const juce::String& message) {
        status_strip_.set_message(message);
        status_age_ = 0;
    };
    context_.show_plugin_window = [this](const std::string& id) { show_plugin_window(id); };
    restore_monitor(context_);
    restore_midi_input_filter(context_);
    context_.show_insert_window = [this](const std::string& owner, int slot) { show_insert_window(owner, slot); };
    context_.capture_plugin_states = [this] { capture_plugin_states(); };
    context_.show_comment = [this](const std::string& id) { review::show_comments(context_, id); };
    context_.assign_from_libraries = [this](const std::string& prefer) {
        if (!context_.library) return;
        context_.library->refresh();
        context_.status(context_.library->assign_from_libraries(prefer));
    };
    context_.installed_libraries = [this] {
        return context_.library ? context_.library->installed_libraries() : std::vector<std::pair<std::string, std::string>>{};
    };
    context_.show_library_for_track = [this] {
        set_library_shown(true);
        tracks_.library().set_view(LibraryBrowser::View::ForTrack);
    };

    listener_ = context_.session.add_listener([this](session::Change c) {
        // A MIDI keyboard plays whichever track is selected.
        if (c == session::Change::Selection) {
            context_.engine.set_live_track(context_.session.selected_track());
            // The selection's range shows on the ruler (and punch follows it).
            const auto& sel = context_.session.selection();
            const auto r = sequencer_.range();
            if (r.first != sel.from || r.second != sel.to) {
                syncing_range_ = true;
                sequencer_.set_range(sel.from, sel.to);
                syncing_range_ = false;
                if (punch_) set_punch(true);
            }
            // A loop follows the range; clearing the range ends it.
            if (context_.engine.loop() && context_.engine.loop_range() != std::make_pair(sel.from, sel.to)) {
                if (sel.has_range()) context_.engine.set_loop(true, sel.from, sel.to);
                else set_loop(false);
            }
        }
        if (c == session::Change::Document) {
            // Instrument ranges: say so when notes land outside one (an
            // import, a new instrument, material pasted or moved), without
            // touching them.
            {
                int added = 0;
                juce::String where;
                std::map<std::string, std::set<notation::EventId>> flagged;
                for (const auto& t : context_.session.data().tracks) {
                    auto& now = flagged[t.id];
                    for (const auto& i : context_.session.range_issues(t)) now.insert(i.id);
                    const auto it = range_counts_.find(t.id);
                    int fresh = 0;
                    for (auto id : now)
                        if (it == range_counts_.end() || !it->second.count(id)) ++fresh;
                    if (fresh > 0 && range_counted_) {
                        added += fresh;
                        if (where.isEmpty()) {
                            const auto limits = context_.session.range_limits(t);
                            where = juce::String(limits.instrument.empty() ? t.name : limits.instrument) + "'s range (" +
                                    juce::String(t.name) + ")";
                        }
                    }
                }
                range_counts_ = std::move(flagged);
                range_counted_ = true;
                if (added > 0)
                    context_.status(juce::String(added) + (added == 1 ? " note is" : " notes are") + " outside the " + where +
                                    ", ringed. Track > Check Instrument Ranges to fix.");
            }
            // Close plug-in windows whose plug-in changed or went away.
            for (auto it = plugin_windows_.begin(); it != plugin_windows_.end();) {
                const auto& w = *it->second;
                auto* now = w.slot() < 0 ? context_.engine.plugin_for(w.owner())
                                         : context_.engine.insert_plugin(w.owner(), w.slot());
                if (now != it->second->instance()) it = plugin_windows_.erase(it);
                else ++it;
            }
            // The racks of built-in gear follow the document.
            console::refresh_racks(context_);
        }
    });

    context_.engine.on_transport_changed = [this] { deck_.update_position(); };
    // A recording pass was placed (one undo step), or failed.
    context_.engine.on_recording_finished = [this](const juce::String& message, bool ok) {
        context_.status(message);
        deck_.update_position();
        if (!ok)
            juce::AlertWindow::showAsync(juce::MessageBoxOptions::makeOptionsOk(juce::MessageBoxIconType::WarningIcon,
                                                                                "Recording", message, {}, this),
                                         [](int) {});
    };
    // The ruler's range is the shared selection's range (docs/spec/ui): a
    // drag on the ruler, or notes selected in either view, set it.
    sequencer_.on_range_changed = [this] {
        if (syncing_range_) return;
        const auto r = sequencer_.range();
        context_.session.set_selection_range(r.first, r.second);
        if (punch_) set_punch(true);  // follow the new range, or turn off when it was cleared
    };
    // Follow, as it was left.
    context_.follow = hw::ui_settings().getBoolValue("follow", true);
    edit_.refresh_follow();
    // The keyboard profile chosen last time.
    if (const auto saved = hw::ui_settings().getValue("shortcut_profile"); saved.isNotEmpty())
        use_shortcut_profile(saved.toStdString());
    // External jog/shuttle: the views follow where it puts the playhead.
    surface_ = std::make_unique<surface_ui::ControlSurface>(context_);
    mixer_controller_ = std::make_unique<MixerController>(context_);
    surface_->on_moved = [this](Ticks tick, bool on_grid) {
        if (composer_.isVisible()) composer_.follow_jog(tick, on_grid);
        if (sequencer_.isVisible()) sequencer_.reveal(tick);
    };
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    if (deck_window_) {
        // The window's host holds the deck (a member here): let go of it first.
        deck_window_->clearContentComponent();
        deck_window_.reset();
    }
    // The Inspector window shows components this owns: it goes first.
    delete inspector_window_.getComponent();
    stopTimer();
    if (autosave_thread_.joinable()) autosave_thread_.join();
    surface_.reset();
    mixer_controller_.reset();
    close_video_window();
    context_.session.remove_listener(listener_);
    context_.engine.on_transport_changed = nullptr;
    context_.engine.on_recording_finished = nullptr;
    plugin_windows_.clear();
    console::close_racks();
    console::close_section_zooms();
    context_.show_insert_window = nullptr;
    context_.capture_plugin_states = nullptr;
    context_.show_comment = nullptr;
    context_.show_library_for_track = nullptr;
    context_.assign_from_libraries = nullptr;
    context_.installed_libraries = nullptr;
    context_.freeze_tracks = nullptr;
    context_.track_state = nullptr;
    context_.freeze_progress = nullptr;
    context_.cancel_freeze = nullptr;
    context_.show_instrument_usage = nullptr;
    context_.go_to_start = nullptr;
    collab::Controller::get().detach();
    collab::Controller::get().ask_conflicts = nullptr;
}

void MainComponent::paint(juce::Graphics& g) { g.fillAll(hw::tok::chassis); }

void MainComponent::resized()
{
    constexpr int kPad = 10, kGap = hw::tok::gap_panel, kRight = 268;
    auto r = getLocalBounds();
    status_strip_.setBounds(r.removeFromBottom(18));
    r.reduce(kPad, kPad);
    deck_.setBounds(r.removeFromTop(shell::TopDeck::height_for(getWidth())));
    // The bottom deck: the keyboard (as tall as its key zoom needs), or in
    // the Producer view the mixer.
    // Popped out, the mixer is in its own window and the keyboard stays here.
    const bool mixer_deck = workspace_ == Workspace::Producer && bottom_mixer_ && !deck_window_;
    keyboard_.setVisible(!mixer_deck && !dock_hidden_);
    if (!deck_window_) mixer_deck_.setVisible(mixer_deck && !dock_hidden_);
    keyboard_.show_mixer_key(workspace_ == Workspace::Producer);
    auto& deck = mixer_deck ? static_cast<juce::Component&>(mixer_deck_) : keyboard_;
    if (dock_hidden_) {
        // Hidden: a thin bar with the key that brings it back.
        auto bar = r.removeFromBottom(24);
        dock_key_.setButtonText(mixer_deck ? "Show Mixer" : "Show Keyboard");
        dock_key_.setBounds(bar.withSizeKeepingCentre(130, 22));
    } else {
        deck.setBounds(r.removeFromBottom(mixer_deck ? shell::MixerDeck::kHeight : keyboard_.preferred_height()));
        dock_key_.setButtonText("Hide");
        dock_key_.setBounds(deck.getX() + deck.getWidth() / 2 - 32, deck.getY() + 3, 64, 18);
    }
    dock_key_.toFront(false);
    r.removeFromBottom(kGap);
    r.removeFromTop(kGap);
    toolbar_.setBounds(r.removeFromTop(60));
    r.removeFromTop(kGap);

    if (work_view_) {
        // Work view: no side panels; the edit window takes the width.
        for (juce::Component* c : {static_cast<juce::Component*>(&master_), static_cast<juce::Component*>(&channel_),
                                   static_cast<juce::Component*>(&print_), static_cast<juce::Component*>(&channel_tab_),
                                   static_cast<juce::Component*>(&print_tab_)})
            c->setVisible(false);
        right_tabs_ = false;
        edit_.setBounds(r);
        const auto content = edit_.getBounds().getIntersection(edit_.content_area() + edit_.getPosition());
        composer_.setBounds(content);
        page_view_.setBounds(content);
        sequencer_.setBounds(content);
        return;
    }

    const bool producer = workspace_ == Workspace::Producer;
    // Under ~1280 px the right panel drops below the edit window. The
    // Composer has none: its inspector floats (Window > Inspector).
    const bool narrow = getWidth() < 1280;
    juce::Rectangle<int> right_area;
    if (!narrow && producer) {
        right_area = r.removeFromRight(kRight);
        r.removeFromRight(kGap);
    }
    // Left rack | edit window, with the divider between them.
    juce::Component* items[] = {&markers_, divider_.get(), &edit_};
    layout_.layOutComponents(items, 3, r.getX(), r.getY(), r.getWidth(), r.getHeight(), false, true);
    auto rack = markers_.getBounds();
    divider_->setBounds(divider_->getBounds().reduced(1, 0));
    // The rack: Composer = tracks, then markers; Producer = new track, then tracks.
    if (producer) {
        // New track, the tracks, then the clips (docs/spec/ui, Producer left rack).
        new_track_.setBounds(rack.removeFromTop(200));
        rack.removeFromTop(kGap);
        clips_.setBounds(rack);
    } else {
        // The tracks are the edit window's own lanes; the library floats (Window > Library).
        markers_.setBounds(rack);
    }
    auto edit = edit_.getBounds();
    if (narrow && producer) {
        right_area = edit.removeFromBottom(330);
        edit.removeFromBottom(kGap);
        edit_.setBounds(edit);
    }
    right_tabs_ = false;
    master_.setVisible(producer && master_stage_);
    channel_.setVisible(producer && !master_stage_);
    print_.setVisible(producer && !master_stage_);
    if (producer && master_stage_) {
        // The Master stage: the Master panel fills the column.
        master_.setBounds(narrow ? right_area.withWidth(std::min(right_area.getWidth(), 2 * kRight)) : right_area);
    } else if (producer) {
        // The channel strip, then Print to audio under it.
        if (narrow) {
            channel_.setBounds(right_area.removeFromLeft(kRight));
            right_area.removeFromLeft(kGap);
            print_.setBounds(right_area.withWidth(std::min(right_area.getWidth(), kRight)));
        } else if (right_area.getHeight() < 300 + kGap + 252) {
            // Too short for both: Channel | Print tabs, one panel at a time.
            right_tabs_ = true;
            auto tabs = right_area.removeFromTop(24);
            channel_tab_.setBounds(tabs.removeFromLeft(tabs.getWidth() / 2 - 3));
            tabs.removeFromLeft(6);
            print_tab_.setBounds(tabs);
            right_area.removeFromTop(kGap);
            channel_.setVisible(right_tab_ == 0);
            print_.setVisible(right_tab_ == 1);
            channel_.setBounds(right_area.withHeight(std::min(right_area.getHeight(), 420)));
            print_.setBounds(right_area.withHeight(std::min(right_area.getHeight(), 360)));
        } else {
            // The channel strip, then Print to audio under it.
            const int print_h = juce::jlimit(252, 360, right_area.getHeight() - kGap - 300);
            print_.setBounds(right_area.removeFromBottom(print_h));
            right_area.removeFromBottom(kGap);
            channel_.setBounds(right_area.withHeight(std::min(right_area.getHeight(), 420)));
        }
    }
    channel_tab_.setVisible(right_tabs_);
    print_tab_.setVisible(right_tabs_);
    channel_tab_.setToggleState(right_tab_ == 0, juce::dontSendNotification);
    print_tab_.setToggleState(right_tab_ == 1, juce::dontSendNotification);
    const auto content = edit_.getBounds().getIntersection(edit_.content_area() + edit_.getPosition());
    composer_.setBounds(content);
    page_view_.setBounds(content);
    sequencer_.setBounds(content);
}

void MainComponent::set_follow(bool on)
{
    context_.follow = on;
    hw::ui_settings().setValue("follow", on);
    hw::ui_settings().saveIfNeeded();
    edit_.refresh_follow();
    context_.commands.commandStatusChanged();
    context_.status(on ? "Follow on: the edit window scrolls with the playhead" : "Follow off: the edit window stays where you put it");
}

void MainComponent::set_inspector_shown(bool shown)
{
    hw::ui_settings().setValue("inspector_shown", shown);
    if (shown) {
        if (inspector_window_ == nullptr) {
            const int w = InspectorBox::kWidth, h = InspectorBox::height();
            auto* win = new dialogs::ToolWindow("Inspector", inspector_box_.get(), w, h, false);
            win->setResizeLimits(w, 200, w + 200, 2000);
            juce::Component::SafePointer<MainComponent> self(this);
            win->on_close = [self] {
                if (self == nullptr) return;
                hw::ui_settings().setValue("inspector_shown", false);
                self->context_.commands.commandStatusChanged();
            };
            // Where it was last (any connected display), else beside the edit window.
            const auto saved = juce::StringArray::fromTokens(hw::ui_settings().getValue("inspector_bounds"), " ", "");
            juce::Rectangle<int> r;
            if (saved.size() == 4)
                r = {saved[0].getIntValue(), saved[1].getIntValue(), saved[2].getIntValue(), saved[3].getIntValue()};
            bool on_screen = false;
            for (const auto& d : juce::Desktop::getInstance().getDisplays().displays)
                on_screen = on_screen || d.userArea.intersects(r.reduced(20));
            if (!on_screen) {
                const auto me = getScreenBounds();
                r = juce::Rectangle<int>(w, h).withPosition(me.getRight() - w - 30, me.getY() + 140);
            }
            win->setBounds(r);
            inspector_window_ = win;
        }
        inspector_window_->setVisible(true);
        inspector_window_->setMinimised(false);
        inspector_window_->toFront(false);
    } else if (inspector_window_ != nullptr) {
        inspector_window_->closeButtonPressed();
    }
    context_.commands.commandStatusChanged();
}

bool MainComponent::inspector_shown() const { return inspector_window_ != nullptr && inspector_window_->isVisible(); }

void MainComponent::set_library_shown(bool shown)
{
    if (shown) {
        if (library_window_ == nullptr) {
            auto& lib = tracks_.library();
            lib.setVisible(true);
            auto* win = new dialogs::ToolWindow("Library", &lib, 380, 640, false);
            win->setResizeLimits(300, 320, 1400, 3000);
            juce::Component::SafePointer<MainComponent> self(this);
            juce::Component::SafePointer<dialogs::ToolWindow> safe(win);
            win->on_close = [self, safe] {
                if (self == nullptr) return;
                if (safe != nullptr) {
                    hw::ui_settings().setValue("library_bounds", safe->getBounds().toString());
                    hw::ui_settings().saveIfNeeded();
                }
                self->context_.commands.commandStatusChanged();
            };
            // Where it was last (any connected display), else at the edit window's left.
            const auto r = juce::Rectangle<int>::fromString(hw::ui_settings().getValue("library_bounds"));
            bool on_screen = false;
            for (const auto& d : juce::Desktop::getInstance().getDisplays().displays)
                on_screen = on_screen || d.userArea.intersects(r.reduced(20));
            win->setBounds(on_screen ? r : juce::Rectangle<int>(380, 640).withPosition(getScreenX() + 30, getScreenY() + 160));
            library_window_ = win;
        }
        library_window_->setVisible(true);
        library_window_->setMinimised(false);
        library_window_->toFront(true);
    } else if (library_window_ != nullptr) {
        library_window_->closeButtonPressed();
    }
    context_.commands.commandStatusChanged();
}

bool MainComponent::library_shown() const { return library_window_ != nullptr && library_window_->isVisible(); }

void MainComponent::show_right_tab(int tab)
{
    right_tab_ = tab;
    resized();
}

void MainComponent::apply_side_visibility()
{
    const bool producer = workspace_ == Workspace::Producer, side = !work_view_;
    markers_.setVisible(side && !producer);
    new_track_.setVisible(side && producer);
    clips_.setVisible(side && producer);
    tracks_.setVisible(false);  // every track is in the edit window; the library floats
    if (divider_) divider_->setVisible(side);
}

void MainComponent::set_dock_hidden(bool hidden)
{
    dock_hidden_ = hidden;
    resized();
    context_.commands.commandStatusChanged();
}

void MainComponent::set_work_view(bool on)
{
    work_view_ = on;
    toolbar_.set_work_view(on);
    apply_side_visibility();
    resized();
    context_.commands.commandStatusChanged();
    context_.status(on ? "Work view: side panels hidden (View > Work View or Cmd/Ctrl+Alt+W brings them back)"
                       : "Side panels shown");
}

void MainComponent::toggle_track_list()
{
    if (track_list_open()) close_track_list();
    else show_track_list(context_, [this](const std::string& id) { go_to_track(id); });
    context_.commands.commandStatusChanged();
}

bool MainComponent::track_list_shown() const { return track_list_open(); }

void MainComponent::go_to_track(const std::string& track_id)
{
    context_.session.select_track(track_id);
    if (workspace_ == Workspace::Composer && !page_mode_) composer_.reveal_track(track_id);
    else if (workspace_ == Workspace::Producer) sequencer_.reveal_track(track_id);
    if (const auto* t = context_.session.data().find_track(track_id)) context_.status("Track: " + juce::String(t->name));
}

layouts::ViewLayout MainComponent::capture_layout() const
{
    layouts::ViewLayout l;
    l.producer = workspace_ == Workspace::Producer;
    l.page_view = page_mode_;
    l.bottom_mixer = bottom_mixer_;
    l.dock_hidden = dock_hidden_;
    l.work_view = work_view_;
    l.mixer_full = false;  // the full mixer is the window (console_window)
    l.console_window = mixer_ != nullptr && mixer_->isVisible();
    l.console_display = console_display_;
    // The floating windows, open or not, and where.
    l.has_windows = true;
    auto window = [](juce::DocumentWindow* w, bool shown) {
        layouts::FloatingWindow f;
        f.shown = shown && w != nullptr && w->isVisible();
        if (w != nullptr) f.bounds = w->getBounds();
        return f;
    };
    l.inspector = window(inspector_window_.getComponent(), inspector_shown());
    auto& composer = const_cast<ComposerView&>(composer_);
    l.staff_tools = window(composer.staff_tools_window(), composer.staff_tools_detached());
    l.staff_tools.shown = composer.staff_tools_detached();  // its window shows itself a moment later
    l.track_list = window(track_list_window(), track_list_open());
    return l;
}

void MainComponent::apply_layout(const layouts::ViewLayout& l)
{
    page_mode_ = l.page_view;
    toolbar_.set_page_view(l.page_view);
    work_view_ = l.work_view;
    dock_hidden_ = l.dock_hidden;
    bottom_mixer_ = l.bottom_mixer;
    show_workspace(l.producer ? Workspace::Producer : Workspace::Composer);
    // A layout's full mixer (older layouts say "mixer_full") is the mixer window.
    if (l.console_window || l.mixer_full) show_console_window(l.console_display);
    else if (mixer_ != nullptr) mixer_->setVisible(false);
    if (l.has_windows) {
        // Each floating window opened or closed as saved, and put back where
        // it was - on another display too, while that display is connected.
        auto place = [](juce::DocumentWindow* w, const layouts::FloatingWindow& f) {
            if (w == nullptr || f.bounds.isEmpty()) return;
            for (const auto& d : juce::Desktop::getInstance().getDisplays().displays)
                if (d.userArea.intersects(f.bounds.reduced(20))) {
                    w->setBounds(f.bounds);
                    return;
                }
        };
        set_inspector_shown(l.inspector.shown);
        if (l.inspector.shown) place(inspector_window_.getComponent(), l.inspector);
        composer_.set_staff_tools_detached(l.staff_tools.shown);
        if (l.staff_tools.shown) place(composer_.staff_tools_window(), l.staff_tools);
        if (l.track_list.shown != track_list_open()) toggle_track_list();
        if (l.track_list.shown) place(track_list_window(), l.track_list);
    }
    resized();
    context_.commands.commandStatusChanged();
    if (l.name.isNotEmpty()) context_.status("Layout: " + l.name);
}

juce::PopupMenu MainComponent::layouts_menu()
{
    juce::PopupMenu m;
    const auto now = capture_layout();
    auto add = [&](const layouts::ViewLayout& l) {
        auto bare = l;
        bare.name = {};
        m.addItem(l.name, true, bare == now, [this, l] { apply_layout(l); });
    };
    for (const auto& l : layouts::built_in()) add(l);
    const auto own = layouts::saved();
    if (!own.empty()) {
        m.addSeparator();
        for (const auto& l : own) add(l);
    }
    m.addSeparator();
    m.addCommandItem(&context_.commands, cmd::saveLayout);
    if (!own.empty()) {
        juce::PopupMenu del;
        for (const auto& l : own) {
            const auto name = l.name;
            del.addItem(name, [this, name] {
                layouts::remove(name);
                context_.status("Deleted the layout " + name);
            });
        }
        m.addSubMenu("Delete Layout", del);
    }
    juce::PopupMenu open_with;
    const auto chosen = layouts::startup();
    open_with.addItem("The layout it was left in", true, chosen.isEmpty(), [] { layouts::set_startup({}); });
    open_with.addSeparator();
    for (const auto& l : layouts::built_in()) {
        const auto name = l.name;
        open_with.addItem(name, true, chosen == name, [name] { layouts::set_startup(name); });
    }
    for (const auto& l : own) {
        const auto name = l.name;
        open_with.addItem(name, true, chosen == name, [name] { layouts::set_startup(name); });
    }
    m.addSubMenu("Open VC Composer With", open_with);
    return m;
}

void MainComponent::save_layout_as()
{
    auto* w = new juce::AlertWindow("Save Layout",
                                    "Saves how the window is arranged now: the view, the bottom deck, the side panels "
                                    "and the mixer (and the display it is on).",
                                    juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", "My Layout", "Name");
    w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int result) {
                           const auto name = w->getTextEditorContents("name").trim();
                           if (result == 1 && name.isNotEmpty()) {
                               auto l = capture_layout();
                               l.name = name;
                               layouts::save(l);
                               context_.status("Saved the layout " + name + " (View > Layouts)");
                           }
                       }),
                       true);
}

void MainComponent::show_bottom_mixer(bool mixer)
{
    bottom_mixer_ = mixer;
    if (mixer && deck_window_) {
        // It is in its own window: bring that forward.
        deck_window_->setVisible(true);
        deck_window_->toFront(true);
    }
    resized();
}

namespace {
/// Holds the deck in its window: an opaque background under it, so nothing
/// of the window shows through while parts of the mixer repaint.
class DeckHost final : public juce::Component {
public:
    explicit DeckHost(juce::Component& deck) : deck_(deck)
    {
        setOpaque(true);
        addAndMakeVisible(deck_);
    }
    ~DeckHost() override { removeChildComponent(&deck_); }
    void paint(juce::Graphics& g) override { g.fillAll(hw::tok::chassis); }
    void resized() override { deck_.setBounds(getLocalBounds()); }

private:
    juce::Component& deck_;
};

/// The mixer deck's own window: closing it docks the mixer again.
class DeckWindow final : public juce::DocumentWindow {
public:
    explicit DeckWindow(std::function<void()> dock)
        : juce::DocumentWindow("Mixer", hw::tok::chassis, juce::DocumentWindow::allButtons), dock_(std::move(dock))
    {
        setUsingNativeTitleBar(true);
        setResizable(true, false);
        setResizeLimits(480, shell::MixerDeck::kHeight, 10000, 10000);
    }
    void closeButtonPressed() override
    {
        // Docked from a callback, not while the window is mid-close.
        juce::MessageManager::callAsync([d = dock_] { d(); });
    }

private:
    std::function<void()> dock_;
};
}  // namespace

void MainComponent::set_mixer_popped(bool popped)
{
    if (popped == mixer_popped()) {
        if (deck_window_) deck_window_->toFront(true);
        return;
    }
    if (popped) {
        juce::Component::SafePointer<MainComponent> self(this);
        auto window = std::make_unique<DeckWindow>([self] {
            if (self) self->set_mixer_popped(false);
        });
        removeChildComponent(&mixer_deck_);
        mixer_deck_.set_floating(true);
        mixer_deck_.setVisible(true);
        window->setContentOwned(new DeckHost(mixer_deck_), false);
        // Big enough for every strip in full - inserts, pan, a long fader -
        // as wide as the strips need, within the screen.
        const auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
        const int strips_w = 32 + mixer_deck_.strip_count() * (shell::MixerDeck::kStripFull + 4) + shell::MixerDeck::kMasterStripFull + 16;
        const int want_w = std::clamp(strips_w, 720, area.getWidth() * 95 / 100);
        const int want_h = std::min(kPoppedMixerHeight, area.getHeight() * 90 / 100);
        window->setResizeLimits(560, std::min(kPoppedMixerMinHeight, want_h), 10000, 10000);
        // Where it was left (another screen, say), else centred here.
        const auto saved = hw::ui_settings().getValue("mixer_deck_window");
        if (saved.isEmpty() || !window->restoreWindowStateFromString(saved)) window->centreWithSize(want_w, want_h);
        if (window->getHeight() < want_h * 9 / 10)
            window->setSize(window->getWidth(), want_h);  // left too short: everything shows again
        window->setVisible(true);
        use_steady_renderer(*window);
        window->toFront(true);
        deck_window_ = std::move(window);
        context_.status("Mixer in its own window - drag it to another screen; Dock (or closing it) brings it back");
    } else {
        hw::ui_settings().setValue("mixer_deck_window", deck_window_->getWindowStateAsString());
        hw::ui_settings().saveIfNeeded();
        deck_window_->clearContentComponent();  // the host lets go of the deck
        deck_window_.reset();
        mixer_deck_.set_floating(false);
        addChildComponent(mixer_deck_);
        if (workspace_ != Workspace::Producer) show_workspace(Workspace::Producer);
        bottom_mixer_ = true;
        context_.status("Mixer back in the bottom deck");
    }
    resized();
}

void MainComponent::go_to_stage(int stage)
{
    switch (stage) {
        case 0: set_page_view(false); break;  // Compose: writing, in the Track view
        case 1: {
            // Score: the full score on pages, with parts and the full score to print.
            set_page_view(true);
            edit_.set_stage(1);
            juce::PopupMenu m;
            m.addItem("Export parts (PDF)...", [this] { export_parts_pdf_dialog(); });
            m.addItem("Export full score (PDF)...", [this] { export_full_score_dialog(); });
            m.addItem("Export MusicXML...", [this] { export_musicxml(); });
            m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&edit_.stage_key(1)));
            break;
        }
        case 2:
            // Mix: the Producer view with the mixer in the bottom deck.
            show_workspace(Workspace::Producer);
            master_stage_ = false;
            show_bottom_mixer(true);
            break;
        case 3:
            // Master: the Master panel (loudness, chain, export) in the right column.
            show_workspace(Workspace::Producer);
            master_stage_ = true;
            edit_.set_stage(3);
            resized();
            break;
        default: break;
    }
}

void MainComponent::show_workspace(Workspace w)
{
    // One project, two views: the playhead, the selection and each view's
    // own zoom and scroll stay as they are.
    workspace_ = w;
    const bool producer = w == Workspace::Producer;
    composer_.setVisible(!producer && !page_mode_);
    page_view_.setVisible(!producer && page_mode_);
    sequencer_.setVisible(producer);
    apply_side_visibility();
    master_stage_ = false;  // a view switch leaves the Master stage
    deck_.set_producer(producer);
    toolbar_.set_producer(producer);
    edit_.set_stage(producer ? 2 : 0);
    if (producer) channel_.refresh();
    resized();
    if (w == Workspace::Composer) {
        if (page_mode_) page_view_.grabKeyboardFocus();
        else composer_.grabKeyboardFocus();
    } else {
        sequencer_.grabKeyboardFocus();
    }
}

void MainComponent::set_page_view(bool page)
{
    page_mode_ = page;
    toolbar_.set_page_view(page);
    if (workspace_ != Workspace::Composer) show_workspace(Workspace::Composer);
    else show_workspace(Workspace::Composer);  // shows the one and hides the other
    context_.status(page ? "Score view: the full score as it prints and exports - click a system to move the playhead there"
                         : "Track view");
}

void MainComponent::refresh_tool_keys()
{
    toolbar_.update(punch_, sequencer_.pencil_mode(), static_cast<int>(sequencer_.edit_mode()), sequencer_.smart_tool());
    if (auto* k = toolbar_.key("Chord mode")) k->setToggleState(composer_.chord_mode(), juce::dontSendNotification);
    if (auto* k = toolbar_.key("Hear")) k->setToggleState(composer_.hear_entry(), juce::dontSendNotification);
}

void MainComponent::timerCallback()
{
    const Ticks tick = context_.engine.position();
    const bool playing = context_.engine.playing();
    // Save on transport stop (Main Spec §12): a saved project, changed, saves
    // when playback stops (never while recording or playing).
    if (was_playing_ && !playing && !context_.engine.recording() && hw::ui_settings().getBoolValue("save_on_stop", false) &&
        !context_.session.path().empty() && context_.session.dirty() && !preview::ended() && Account::get().saving().can_save) {
        std::string error;
        if (context_.session.save(&error)) context_.status("Saved on stop");
        else context_.status("Could not save on stop: " + juce::String(error));
    }
    was_playing_ = playing;
    // The licence's standing, asked in the background a minute after start
    // and every six hours after (a failure changes nothing).
    {
        static double next_check = juce::Time::getMillisecondCounterHiRes() + 60'000.0;
        auto& account = Account::get();
        if (juce::Time::getMillisecondCounterHiRes() >= next_check) {
            next_check = juce::Time::getMillisecondCounterHiRes() + 6.0 * 3600'000.0;
            if (account.configured() && account.signed_in()) juce::Thread::launch([] { Account::get().check_standing(); });
        }
    }
    deck_.update_position();
    const auto& sel = context_.session.selection();
    deck_.set_range(sel.from, sel.to);
    refresh_tool_keys();
    if (page_view_.isVisible()) page_view_.set_playhead(tick);
    if (composer_.isVisible()) {
        composer_.set_playhead(tick, playing);
        // The Composer keys light from the editor: entry, value, dot, rest, voice.
        if (const auto cur = composer_.cursor()) {
            toolbar_.set_entry_state(composer_.note_entry(), cur->duration, cur->rest_mode);
            inspector_.set_voice(cur->voice);
        } else {
            toolbar_.set_entry_state(composer_.note_entry(), notation::Duration{}, false);
            inspector_.set_voice(-1);
        }
    }
    if (sequencer_.isVisible()) sequencer_.set_playhead(tick, playing);
    // An edit refused by an approved version: say why, and drop what the
    // Composer had written on its working copy.
    if (const auto refused = context_.session.project().refusal_count(); refused != refusals_seen_) {
        if (refused > refusals_seen_ && !context_.session.last_refusal().empty()) {
            context_.status("Not changed: " + juce::String(context_.session.last_refusal()));
            composer_.discard_uncommitted();
            // One articulation per bar: offer a new part for the overlap.
            if (const std::string part = context_.session.refused_part(); !part.empty()) offer_new_part(part);
        }
        refusals_seen_ = refused;
    }
    // A plug-in that crashed in its own process: the session plays on without it.
    if (const auto crashed = context_.engine.take_newly_crashed_plugins(); !crashed.isEmpty())
        context_.status(crashed.joinIntoString(", ") + (crashed.size() == 1 ? " has" : " have") +
                        " crashed and gone silent; everything else keeps playing. Tools > Reload Crashed Plug-ins "
                        "starts it again from its last state.");
    if (const int late = context_.engine.take_late_plugin_blocks(); late > 0 && playing)
        context_.status("A plug-in could not keep up (" + juce::String(late) + " short dropout" +
                        (late == 1 ? "" : "s") + "); a larger audio buffer gives it more time");
    // Never silent: a disk that cannot keep up with the audio clips is said.
    if (const int dropouts = context_.engine.take_disk_underruns(); dropouts > 0)
        context_.status("The disk could not keep up with the audio clips (" + juce::String(dropouts) +
                        " short dropout" + (dropouts == 1 ? "" : "s") + ")");

    // Crash protection: an autosave every minute while there are unsaved changes.
    if (++ticks_ >= 30 * 60) {
        ticks_ = 0;
        if (context_.session.dirty() && !autosave_running_.load()) {
            capture_plugin_states();
            // The copy now, the writing on its own thread.
            auto job = std::make_shared<session::Session::AutosaveJob>(
                context_.session.autosave_job(user_data_directory().getFullPathName().toStdString()));
            if (autosave_thread_.joinable()) autosave_thread_.join();
            autosave_running_ = true;
            autosave_thread_ = std::thread([this, job] {
                session::Session::write_autosave_job(*job);
                autosave_running_ = false;
            });
            status_strip_.set_autosaved(juce::Time::getCurrentTime());
        }
    }
    if (++status_age_ == 30 * 8) status_strip_.set_message({});
}

// --- File ------------------------------------------------------------------------------------------

std::function<bool(const juce::File&)> MainComponent::open_shared_file()
{
    juce::Component::SafePointer<MainComponent> self(this);
    return [self](const juce::File& f) {
        if (self == nullptr || !self->confirm_discard()) return false;
        self->open_file(f);
        return self->context_.session.path() == f.getFullPathName().toStdString();
    };
}

bool MainComponent::confirm_discard()
{
    if (!context_.session.dirty()) return true;
    const int r = juce::AlertWindow::showYesNoCancelBox(
        juce::MessageBoxIconType::QuestionIcon, "Unsaved changes",
        "Save the changes to \"" + juce::String(context_.session.title()).upToLastOccurrenceOf(" *", false, false) + "\"?",
        "Save", "Don't save", "Cancel", this, nullptr);
    if (r == 0) return false;
    if (r == 1) return save(false);
    return true;
}

void MainComponent::open_package(const juce::File& zip)
{
    juce::String error;
    const auto project = review::unpack(zip, error);
    if (project == juce::File()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not open the package", error);
        return;
    }
    open_file(project);
    context_.status("Opened the package in " + project.getParentDirectory().getFullPathName() +
                    " (its Package Report.txt lists the plug-ins and libraries it needs)");
}


juce::File MainComponent::project_folder_for_dialogs() const
{
    // The project's own folder; else the last place a project was saved or
    // opened; else Documents.
    if (const auto path = context_.session.path(); !path.empty()) {
        const auto dir = juce::File(juce::String(path)).getParentDirectory();
        if (dir.isDirectory()) return dir;
    }
    const juce::File last = recent::last_folder();
    if (last.isDirectory()) return last;
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
}

juce::PopupMenu MainComponent::recent_menu()
{
    juce::PopupMenu m;
    for (const auto& path : recent::projects()) {
        const juce::File f(path);
        m.addItem(f.getFileNameWithoutExtension() + "  -  " + f.getParentDirectory().getFullPathName(), f.existsAsFile(),
                  false, [this, f] {
                      if (confirm_discard()) open_file(f);
                  });
    }
    if (m.getNumItems() == 0) m.addItem("No recent projects", false, false, [] {});
    else {
        m.addSeparator();
        m.addItem("Clear Recent", [] { recent::clear(); });
    }
    return m;
}

void MainComponent::open_file(const juce::File& file)
{
    std::string path = file.getFullPathName().toStdString();
    if (session::Session::has_newer_autosave(path)) {
        const bool recover = juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::QuestionIcon, "Recover unsaved work?",
            "VC Composer found an autosave of this project that is newer than the saved file. Open the autosave?",
            "Open autosave", "Open saved file", this, nullptr);
        if (recover) path += ".autosave";
    }
    std::string error;
    if (!context_.session.open(path, &error)) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not open", error);
        return;
    }
    if (juce::String(path).endsWith(".autosave")) {
        // Recovered: keep the original file as the save target.
        context_.session.edit("Recovered from autosave", [](project::ProjectData&) { return true; });
    }
    context_.engine.locate(0);
    recent::remember(juce::File(juce::String(context_.session.path())));
    const auto missing = session::relink::missing(context_.session.data(), context_.session.path()).size();
    context_.status("Opened " + file.getFileName() +
                    (missing > 0 ? " - " + juce::String(static_cast<int>(missing)) +
                                       " media file(s) missing: File > Relink Missing Media... finds them"
                                 : juce::String()));
    // Plug-ins not installed here: say which, and offer substitutes (never silently).
    show_missing_instruments(context_);
}

void MainComponent::capture_plugin_states()
{
    // Instruments and effect inserts alike, on tracks and buses.
    context_.engine.capture_plugin_states();
}

bool MainComponent::save(bool save_as)
{
    if (!may_save(context_)) return false;
    capture_plugin_states();
    std::string path = context_.session.path();
    if (save_as || path.empty()) {
        // Starts where this project lives (another drive, a cloud folder),
        // or where the last one was saved.
        juce::FileChooser chooser("Save project",
                                  project_folder_for_dialogs().getChildFile(
                                      juce::File::createLegalFileName(juce::String(context_.session.data().name)) + ".vcc"),
                                  "*.vcc");
        if (!chooser.browseForFileToSave(true)) return false;
        path = chooser.getResult().withFileExtension("vcc").getFullPathName().toStdString();
    }
    std::string error;
    if (!context_.session.save_as(path, &error)) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not save", error);
        return false;
    }
    recent::remember(juce::File(juce::String(path)));
    context_.status("Saved " + juce::File(path).getFullPathName());
    return true;
}

void MainComponent::import_musicxml()
{
    chooser_ = std::make_unique<juce::FileChooser>("Import MusicXML", juce::File(), "*.musicxml;*.mxl;*.xml");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                              const auto file = fc.getResult();
                              if (file == juce::File()) return;
                              auto finish = [this, file](bool replace) {
                                  juce::String message;
                                  const bool ok = import_musicxml_file(context_, file, replace, message);
                                  context_.status(message);
                                  if (!ok)
                                      juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                             "Import MusicXML", message);
                              };
                              if (context_.session.data().tracks.empty()) {
                                  finish(true);
                                  return;
                              }
                              juce::AlertWindow::showAsync(
                                  juce::MessageBoxOptions()
                                      .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle("Import MusicXML")
                                      .withMessage("Add the score's parts to this project as new tracks, or start a "
                                                   "new project from the score (tempo, meter and key from the file)?")
                                      .withButton("Add Tracks")
                                      .withButton("Replace Project")
                                      .withButton("Cancel"),
                                  [finish](int r) {
                                      if (r == 1) finish(false);
                                      else if (r == 2) finish(true);
                                  });
                          });
}

void MainComponent::export_musicxml()
{
    chooser_ = std::make_unique<juce::FileChooser>(
        "Export MusicXML",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(juce::File::createLegalFileName(context_.session.data().name) + ".musicxml"),
        "*.musicxml;*.mxl");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& fc) {
                              auto file = fc.getResult();
                              if (file == juce::File()) return;
                              if (!file.hasFileExtension("mxl") && !file.hasFileExtension("musicxml") &&
                                  !file.hasFileExtension("xml"))
                                  file = file.withFileExtension("musicxml");
                              juce::String message;
                              export_musicxml_file(context_, file, false, message);
                              context_.status(message);
                          });
}

void MainComponent::export_parts_pdf_dialog()
{
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Export Parts (PDF)")
            .withMessage("Print each part to its own PDF in a folder, or every part into one PDF with a bookmark per "
                         "part?")
            .withButton("One PDF per Part")
            .withButton("All Parts in One PDF")
            .withButton("Cancel"),
        [this](int r) {
            if (r == 0) return;
            const bool one_file = r == 2;
            const auto docs = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
            chooser_ = one_file ? std::make_unique<juce::FileChooser>(
                                      "Save all parts as",
                                      docs.getChildFile(juce::File::createLegalFileName(context_.session.data().name) +
                                                        " - parts.pdf"),
                                      "*.pdf")
                                : std::make_unique<juce::FileChooser>("Choose a folder for the parts", docs);
            const int mode = one_file ? juce::FileBrowserComponent::saveMode |
                                             juce::FileBrowserComponent::warnAboutOverwriting
                                       : juce::FileBrowserComponent::openMode |
                                             juce::FileBrowserComponent::canSelectDirectories;
            chooser_->launchAsync(mode, [this, one_file](const juce::FileChooser& fc) {
                auto target = fc.getResult();
                if (target == juce::File()) return;
                if (one_file) target = target.withFileExtension("pdf");
                else if (!target.isDirectory()) return;
                juce::String message;
                export_parts_pdf(context_, target, message);
                context_.status(message);
            });
        });
}

void MainComponent::export_full_score_dialog()
{
    auto* w = new juce::AlertWindow("Export Full Score (PDF)", "Every instrument track that holds notation, in track "
                                    "order, bracketed by section.",
                                    juce::MessageBoxIconType::NoIcon);
    w->addComboBox("pitch", {"Transposed score", "Score in C (concert pitch)"}, "Pitch");
    w->addComboBox("paper", {"A3", "Tabloid", "A4", "Letter"}, "Paper");
    w->addComboBox("orientation", {"Portrait", "Landscape"}, "Orientation");
    w->addComboBox("bars", {"As many as fit", "1", "2", "3", "4", "5", "6", "8", "10", "12"}, "Bars per system");
    w->addComboBox("hide", {"Hide staves where a part rests", "Show every staff on every system"}, "Empty staves");
    w->addComboBox("order", {"Orchestral score order", "Track order"}, "Order");
    {
        // As last set (here or in the Page view).
        const auto saved = saved_full_score_settings();
        w->getComboBoxComponent("pitch")->setSelectedItemIndex(saved.concert_pitch ? 1 : 0);
        w->getComboBoxComponent("paper")->setText(juce::String(saved.paper));
        w->getComboBoxComponent("orientation")->setSelectedItemIndex(saved.landscape ? 1 : 0);
        if (saved.bars_per_system <= 0) w->getComboBoxComponent("bars")->setSelectedItemIndex(0);
        else w->getComboBoxComponent("bars")->setText(juce::String(saved.bars_per_system));
        if (w->getComboBoxComponent("bars")->getSelectedItemIndex() < 0) w->getComboBoxComponent("bars")->setSelectedItemIndex(4);
        w->getComboBoxComponent("hide")->setSelectedItemIndex(saved.hide_empty_staves ? 0 : 1);
        w->getComboBoxComponent("order")->setSelectedItemIndex(saved.score_order ? 0 : 1);
    }
    w->addButton("Export...", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int r) {
        if (r != 1) return;
        FullScoreSettings settings;
        settings.concert_pitch = w->getComboBoxComponent("pitch")->getSelectedItemIndex() == 1;
        settings.paper = w->getComboBoxComponent("paper")->getText().toStdString();
        settings.bars_per_system = w->getComboBoxComponent("bars")->getSelectedItemIndex() == 0
                                       ? 0
                                       : w->getComboBoxComponent("bars")->getText().getIntValue();
        settings.landscape = w->getComboBoxComponent("orientation")->getSelectedItemIndex() == 1;
        settings.hide_empty_staves = w->getComboBoxComponent("hide")->getSelectedItemIndex() == 0;
        settings.score_order = w->getComboBoxComponent("order")->getSelectedItemIndex() == 0;
        save_full_score_settings(settings);
        page_view_.relayout();
        const auto docs = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        chooser_ = std::make_unique<juce::FileChooser>(
            "Save the full score as",
            docs.getChildFile(juce::File::createLegalFileName(context_.session.data().name) +
                              (settings.concert_pitch ? " - score in C.pdf" : " - full score.pdf")),
            "*.pdf");
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, settings](const juce::FileChooser& fc) {
                                  auto file = fc.getResult();
                                  if (file == juce::File()) return;
                                  juce::String message;
                                  export_full_score_pdf(context_, file.withFileExtension("pdf"), settings, message);
                                  context_.status(message);
                              });
    }), true);
}

void MainComponent::export_midi()
{
    chooser_ = std::make_unique<juce::FileChooser>("Export MIDI", juce::File(), "*.mid");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& fc) {
                              const auto file = fc.getResult();
                              if (file == juce::File()) return;
                              std::string error;
                              const bool ok = files::write_file_atomic(
                                  file.withFileExtension("mid").getFullPathName().toStdString(),
                                  project::export_project_midi(context_.session.project()), &error);
                              context_.status(ok ? "Exported MIDI (temp tracks left out)" : juce::String("Export failed: ") + error);
                          });
}

namespace {

void export_parts_svg(AppContext& c, const juce::File& folder)
{
    engraving::LayoutEngine engine;
    if (music_font().loaded) engine.set_glyph_table(music_font().glyphs);
    int pages = 0;
    for (const auto* t : c.session.staff_tracks()) {
        const auto* v = t->active_version();
        if (!v || t->kind != project::TrackKind::Instrument) continue;
        engraving::PrintOptions po;
        po.subtitle = t->name;
        apply_score_info(c, po);
        po.multirests = true;  // a part: empty runs as multi-bar rests
        const auto laid = engraving::lay_out_pages(v->score, engine, po);
        const auto svgs = engraving::render_pages_svg(laid, {}, music_font().loaded ? &music_font().glyphs : nullptr);
        for (std::size_t i = 0; i < svgs.size(); ++i) {
            auto f = folder.getChildFile(juce::File::createLegalFileName(juce::String(t->name)) + " - page " +
                                         juce::String(i + 1) + ".svg");
            files::write_file_atomic(f.getFullPathName().toStdString(), svgs[i]);
            ++pages;
        }
    }
    c.status("Exported " + juce::String(pages) + " part page(s) as SVG to " + folder.getFullPathName());
}

}  // namespace

void MainComponent::save_as_template()
{
    auto* w = new juce::AlertWindow("Save as template", "A name for your custom template", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", juce::String(context_.session.data().name));
    w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int r) {
        if (r != 1) return;
        const juce::String name = w->getTextEditorContents("name").trim();
        if (name.isEmpty()) return;
        juce::String slug = name.toLowerCase().retainCharacters("abcdefghijklmnopqrstuvwxyz0123456789 ").replaceCharacter(' ', '_');
        if (slug.isEmpty()) slug = "custom";
        const std::string id = "user." + slug.toStdString();
        auto tpl = templates::save_user_template(context_.session.project(), id, name.toStdString(), "");
        std::string error;
        if (!context_.catalogs.templates->add_user_template(tpl, &error)) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not save template", error);
            return;
        }
        auto folder = user_data_directory().getChildFile("Templates");
        folder.createDirectory();
        const auto pack = context_.catalogs.templates->export_pack({id}, "user");
        files::write_file_atomic(folder.getChildFile(slug + ".json").getFullPathName().toStdString(), json::write(pack));
        context_.status("Saved template \"" + name + "\" - it is in the template browser under Custom");
    }), true);
}

juce::PopupMenu MainComponent::theme_menu()
{
    // The spec's themeable colours (docs/spec/ui/SPEC.md): live, remembered.
    auto& theme = hw::Theme::get();
    juce::PopupMenu m;
    m.addSectionHeader("LED colour");
    for (const auto& c : hw::Theme::led_choices())
        m.addItem(juce::String(c.name) + " LEDs", true, theme.led() == c.colour,
                  [colour = c.colour] { hw::Theme::get().set_led(colour); });
    m.addSectionHeader("LCD colour");
    for (const auto& c : hw::Theme::lcd_choices())
        m.addItem(juce::String(c.name) + " LCDs", true, theme.lcd() == c.colour,
                  [colour = c.colour] { hw::Theme::get().set_lcd(colour); });
    m.addSectionHeader("Staves");
    m.addItem("Dark staves", true, !theme.paper(), [] { hw::Theme::get().set_paper(false); });
    m.addItem("Paper staves", true, theme.paper(), [] { hw::Theme::get().set_paper(true); });
    return m;
}

juce::PopupMenu MainComponent::shortcuts_menu()
{
    juce::PopupMenu m;
    for (const auto& p : all_shortcut_profiles()) {
        const std::string name = p.name();
        juce::String label(name);
        if (name == "Composer 2") label << "  (5 quarter, 4 eighth, 6 half)";
        m.addItem(label, true, context_.shortcuts.name() == name, [this, name] {
            if (use_shortcut_profile(name)) context_.status("Keyboard shortcuts: " + juce::String(name));
        });
    }
    m.addSeparator();
    m.addItem("Edit Shortcuts...", [this] {
        show_shortcut_editor(context_, [this](const std::string& name) { use_shortcut_profile(name); });
    });
    return m;
}

bool MainComponent::use_shortcut_profile(const std::string& name)
{
    for (auto& p : all_shortcut_profiles())
        if (p.name() == name) {
            context_.shortcuts = std::move(p);
            toolbar_.refresh_legends();
            inspector_.refresh_legends();
            hw::ui_settings().setValue("shortcut_profile", juce::String(name));
            hw::ui_settings().saveIfNeeded();
            return true;
        }
    return false;
}

void MainComponent::show_control_gallery()
{
    if (!gallery_window_) {
        class Window final : public juce::DocumentWindow {
        public:
            Window() : juce::DocumentWindow("Control Gallery", hw::tok::chassis, juce::DocumentWindow::closeButton)
            {
                setUsingNativeTitleBar(true);
                setContentOwned(new hw::Gallery(), true);
                setResizable(false, false);
            }
            void closeButtonPressed() override { setVisible(false); }
        };
        gallery_window_ = std::make_unique<Window>();
        gallery_window_->centreWithSize(gallery_window_->getWidth(), gallery_window_->getHeight());
    }
    gallery_window_->setVisible(true);
    gallery_window_->toFront(true);
}

void MainComponent::show_mixer() { set_mixer_full(!mixer_full(), false); }

void MainComponent::set_mixer_full(bool full, bool compact_after)
{
    // The full mixer is never docked: it is a window of its own (move it to
    // another screen; its close button closes it). Only the compact mixer
    // sits in the bottom deck.
    if (full) {
        show_console_window();
        context_.status("Mixer window - close it (or Cmd/Ctrl+M) to put it away; the compact mixer stays in the bottom deck");
        return;
    }
    if (mixer_ != nullptr) mixer_->setVisible(false);
    if (compact_after) {
        // Compact: the mixer in the bottom deck, under the Producer view's tracks and piano roll.
        if (workspace_ != Workspace::Producer) show_workspace(Workspace::Producer);
        bottom_mixer_ = true;
        resized();
    }
}

std::vector<std::pair<juce::String, juce::String>> MainComponent::display_keys()
{
    std::vector<std::pair<juce::String, juce::String>> out;
    const auto& displays = juce::Desktop::getInstance().getDisplays().displays;
    for (int i = 0; i < displays.size(); ++i) {
        const auto& d = displays.getReference(i);
        out.emplace_back(d.totalArea.toString(), "Display " + juce::String(i + 1) + " - " +
                                                     juce::String(d.totalArea.getWidth()) + " x " +
                                                     juce::String(d.totalArea.getHeight()) +
                                                     (d.isMain ? " (main)" : juce::String()));
    }
    return out;
}

void MainComponent::show_console_window(const juce::String& display)
{
    if (!mixer_) {
        mixer_ = std::make_unique<MixerWindow>(context_);
        dialogs::fit_to_screen(*mixer_, 1000, 460);
        // Its Compact key puts it away, back to the mixer in the bottom deck.
        mixer_->mixer().view().set_on_compact([this] { set_mixer_full(false); });
    }
    // Shown first: a first showing restores where it was left, and the chosen display then wins.
    mixer_->setVisible(true);
    if (display.isNotEmpty()) {
        const juce::Displays::Display* target = nullptr;
        for (const auto& d : juce::Desktop::getInstance().getDisplays().displays)
            if (d.totalArea.toString() == display) target = &d;
        if (target != nullptr) {
            // The whole display: a touch screen given over to the mixer.
            mixer_->setBounds(target->userArea);
            console_display_ = display;
            hw::ui_settings().setValue("console_display", display);
            hw::ui_settings().saveIfNeeded();
        } else {
            context_.status("That display is not connected now: the mixer opens on the main one");
        }
    }
    mixer_->setVisible(true);
    mixer_->toFront(true);
}

juce::PopupMenu MainComponent::mixer_display_menu()
{
    juce::PopupMenu m;
    for (const auto& [key, label] : display_keys())
        m.addItem(label, true, key == console_display_, [this, key = key] { show_console_window(key); });
    m.addSeparator();
    const bool at_start = hw::ui_settings().getBoolValue("console_at_startup", false);
    m.addItem("Open Mixer at Start-up", true, at_start, [this, at_start] {
        hw::ui_settings().setValue("console_at_startup", !at_start);
        hw::ui_settings().saveIfNeeded();
        context_.status(!at_start ? "The mixer window will open at start-up, on the display chosen here"
                                  : "The mixer window no longer opens at start-up");
    });
    return m;
}

void MainComponent::show_plugin_window(const std::string& id)
{
    show_plugin_window_when_loaded(id, 0);
}

void MainComponent::show_plugin_window_when_loaded(const std::string& id, int attempt)
{
    auto* instance = context_.engine.plugin_for(id);
    if (!instance) {
        const auto* t = context_.session.data().find_track(id);
        if (!t || t->instrument_plugin.empty()) {
            context_.status("This track is played by the built-in sound; assign an instrument plug-in first");
            return;
        }
        // Just assigned: the plug-in is still loading (a large library can
        // take a while). Open its window the moment it is ready.
        constexpr int kTries = 240, kEveryMs = 250;  // up to a minute
        if (attempt >= kTries) {
            context_.status(juce::String(t->instrument_plugin.name) + " has not loaded yet: check it in Tools > Plug-ins...");
            return;
        }
        if (attempt == 0) context_.status("Loading " + juce::String(t->instrument_plugin.name) + "...");
        juce::Component::SafePointer<MainComponent> self(this);
        juce::Timer::callAfterDelay(kEveryMs, [self, id, attempt] {
            if (self) self->show_plugin_window_when_loaded(id, attempt + 1);
        });
        return;
    }
    auto& w = plugin_windows_["instrument:" + id];
    if (!w || w->instance() != instance) w = std::make_unique<PluginWindow>(context_, id, -1, *instance);
    w->setVisible(true);
    w->toFront(true);
}

std::string MainComponent::insert_kind(const std::string& owner, int slot) const
{
    const std::vector<project::InsertSlot>* inserts = nullptr;
    if (const auto* t = context_.session.data().find_track(owner)) inserts = &t->inserts;
    else if (const auto* b = context_.session.data().find_bus(owner)) inserts = &b->inserts;
    if (!inserts || slot < 0 || slot >= static_cast<int>(inserts->size())) return {};
    return (*inserts)[static_cast<size_t>(slot)].kind;
}

void MainComponent::show_insert_window(const std::string& owner, int slot)
{
    const auto kind = insert_kind(owner, slot);
    if (kind.empty()) return;
    const std::string key = owner + "#" + std::to_string(slot);
    juce::String owner_name;
    if (const auto* t = context_.session.data().find_track(owner)) owner_name = t->name;
    else if (const auto* b = context_.session.data().find_bus(owner)) owner_name = b->name;

    if (kind == "plugin") {
        context_.engine.rebuild_now();  // a slot filled a moment ago has its plug-in now
        auto* instance = context_.engine.insert_plugin(owner, slot);
        if (!instance) {
            const auto why = context_.engine.insert_status(owner, slot);
            context_.status(why.isNotEmpty() ? why : juce::String("That plug-in is not loaded"));
            return;
        }
        auto& w = plugin_windows_["insert:" + key];
        if (!w || w->instance() != instance) w = std::make_unique<PluginWindow>(context_, owner, slot, *instance);
        w->setName(instance->getName() + " - " + owner_name);
        w->setVisible(true);
        w->toFront(true);
        return;
    }
    // Built-in processors: the strip's rack of outboard gear, at this unit.
    console::show_rack(context_, owner, slot);
}

void MainComponent::show_audio_settings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent(context_.engine.devices(), 0, 2, 0, 8, true, false, true, false);
    selector->setSize(520, 420);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(selector);
    o.dialogTitle = "Audio and MIDI settings";
    o.dialogBackgroundColour = colours::background;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::show_plugin_scanner()
{
    // JUCE's plug-in list, with what our scanner knows underneath: how it
    // scans, and why each failed plug-in failed.
    class Scanner final : public juce::Component, private juce::ChangeListener, private juce::Timer {
    public:
        explicit Scanner(AppContext& c)
            : context_(c),
              list_(c.plugins.formats(), c.plugins.known(), c.plugins.dead_mans_pedal(), c.plugins.properties(), true)
        {
            // Out of process, plug-ins are scanned four at a time: one that
            // crashes or hangs only ends its own scanner process.
            if (c.plugins.scans_out_of_process()) list_.setNumberOfThreadsForScanning(4);
            addAndMakeVisible(list_);
            mode_.setFont(juce::FontOptions(12.0f));
            addAndMakeVisible(mode_);
            failures_.setMultiLine(true);
            failures_.setReadOnly(true);
            failures_.setColour(juce::TextEditor::backgroundColourId, colours::panel);
            failures_.setColour(juce::TextEditor::textColourId, colours::accent_warm);
            addAndMakeVisible(failures_);
            hosting_label_.setText("Run plug-ins", juce::dontSendNotification);
            hosting_label_.setColour(juce::Label::textColourId, colours::text_dim);
            addAndMakeVisible(hosting_label_);
            hosting_.addItemList(PluginHost::hosting_mode_names(), 1);
            hosting_.setSelectedItemIndex(static_cast<int>(c.plugins.hosting_mode()), juce::dontSendNotification);
            hosting_.setEnabled(c.plugins.host_tool().existsAsFile());
            hosting_.setTooltip("Outside VC Composer, a plug-in that crashes takes down only its own process. "
                                "Sharing a process uses less memory; a crash there silences everything it carries.");
            hosting_.onChange = [this] {
                context_.plugins.set_hosting_mode(
                    static_cast<PluginHost::HostingMode>(hosting_.getSelectedItemIndex()));
                // Move the open project's plug-ins over now, keeping their state.
                const int n = context_.engine.restart_plugins();
                context_.status("Plug-ins now run: " + hosting_.getText().toLowerCase() +
                                (n > 0 ? " (" + juce::String(n) + " restarted with their settings)" : juce::String()));
                refresh();
            };
            addAndMakeVisible(hosting_);
            addAndMakeVisible(processes_);
            retry_.setButtonText("Retry failed plug-ins");
            retry_.setTooltip("Forget the failures, so the next scan tries them again, and let plug-ins that crashed "
                              "share processes again");
            retry_.onClick = [this] {
                for (const auto& [target, reason] : context_.plugins.scan_failures())
                    context_.plugins.clear_scan_failure(target);
                context_.plugins.known().clearBlacklistedFiles();
                context_.plugins.clear_crash_isolation();
                refresh();
            };
            addAndMakeVisible(retry_);
            // The look through the plug-in folders: at start-up, or now.
            at_startup_.setButtonText("Check plug-in folders at start-up");
            at_startup_.setToggleState(PluginWatch::at_startup(), juce::dontSendNotification);
            at_startup_.onClick = [this] { PluginWatch::set_at_startup(at_startup_.getToggleState()); };
            addAndMakeVisible(at_startup_);
            check_now_.setButtonText("Check Folders Now");
            check_now_.setComponentID("plugin_check_now");
            check_now_.setTooltip("New, updated and removed plug-ins, and any that will not load (crashed, hung, or "
                                  "waiting for a licence or authorisation)");
            check_now_.onClick = [this] {
                auto* watch = plugin_watch();
                if (watch == nullptr || watch->running()) return;
                juce::Component::SafePointer<Scanner> self(this);
                watch->start(
                    [self](const juce::String& p) {
                        if (self) self->report_.setText(p, juce::dontSendNotification);
                    },
                    [self](const PluginReport& r) {
                        if (self) self->show_report(r);
                    });
                report_.setText("Checking plug-ins...", juce::dontSendNotification);
            };
            addAndMakeVisible(check_now_);
            details_.setButtonText("Report...");
            details_.onClick = [this] {
                if (auto* watch = plugin_watch())
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Plug-in check",
                                                           watch->last().details());
            };
            addAndMakeVisible(details_);
            report_.setColour(juce::Label::textColourId, colours::accent);
            addAndMakeVisible(report_);
            if (auto* watch = plugin_watch()) show_report(watch->last());
            c.plugins.known().addChangeListener(this);
            refresh();
            startTimer(1000);  // failures arrive from scanning threads
            setSize(760, 600);
        }
        ~Scanner() override
        {
            context_.plugins.known().removeChangeListener(this);
            context_.plugins.save_list();
            context_.engine.rebuild_now();
        }
        void paint(juce::Graphics& g) override { g.fillAll(colours::background); }
        void resized() override
        {
            auto r = getLocalBounds();
            auto watch_row = r.removeFromTop(32).reduced(8, 4);
            at_startup_.setBounds(watch_row.removeFromLeft(250));
            check_now_.setBounds(watch_row.removeFromLeft(150));
            watch_row.removeFromLeft(6);
            details_.setBounds(watch_row.removeFromRight(80));
            report_.setBounds(watch_row.withTrimmedLeft(4));
            auto foot = r.removeFromBottom(failures_.isVisible() ? 180 : 64).reduced(8, 4);
            auto mode_row = foot.removeFromTop(26);
            hosting_label_.setBounds(mode_row.removeFromLeft(90));
            hosting_.setBounds(mode_row.removeFromLeft(330));
            processes_.setBounds(mode_row.withTrimmedLeft(10));
            auto top = foot.removeFromTop(24);
            retry_.setBounds(top.removeFromRight(170));
            mode_.setBounds(top);
            failures_.setBounds(foot.withTrimmedTop(4));
            list_.setBounds(r);
        }

    private:
        void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }
        void timerCallback() override { refresh(); }
        void refresh()
        {
            const bool out = context_.plugins.scans_out_of_process();
            mode_.setText(out ? "Plug-ins are scanned in a separate process: one that crashes or hangs cannot take VC "
                                "Composer down."
                              : "The plug-in scanner tool is missing, so plug-ins are scanned inside the app (a crasher "
                                "is skipped on the next launch). Reinstall to restore safe scanning.",
                          juce::dontSendNotification);
            mode_.setColour(juce::Label::textColourId, out ? colours::text_dim : colours::missing);
            const auto running = context_.plugins.host_processes();
            int instances = 0;
            for (const auto& [label, n] : running) instances += n;
            processes_.setText(running.empty() ? juce::String()
                                               : juce::String(instances) + " plug-in" + (instances == 1 ? "" : "s") +
                                                     " in " + juce::String(static_cast<int>(running.size())) +
                                                     " process" + (running.size() == 1 ? "" : "es"),
                               juce::dontSendNotification);
            processes_.setColour(juce::Label::textColourId, colours::text_dim);
            juce::String text;
            for (const auto& [target, reason] : context_.plugins.scan_failures())
                text << juce::File::createFileWithoutCheckingPath(target).getFileName() << ": " << reason << "\n";
            const bool show = text.isNotEmpty();
            if (failures_.getText() != text) failures_.setText(text, false);
            retry_.setVisible(show || context_.plugins.known().getBlacklistedFiles().size() > 0);
            if (failures_.isVisible() != show) {
                failures_.setVisible(show);
                resized();
            }
        }

        void show_report(const PluginReport& r)
        {
            report_.setText(r.when == juce::Time() ? juce::String("Not checked yet this session") : r.summary(),
                            juce::dontSendNotification);
            details_.setEnabled(r.when != juce::Time());
        }

        AppContext& context_;
        juce::ToggleButton at_startup_;
        juce::TextButton check_now_, details_;
        juce::Label report_;
        juce::PluginListComponent list_;
        juce::Label mode_;
        juce::TextEditor failures_;
        juce::TextButton retry_;
        juce::ComboBox hosting_;
        juce::Label hosting_label_, processes_;
    };
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(new Scanner(context_));
    o.dialogTitle = "Instrument and effect plug-ins";
    o.dialogBackgroundColour = colours::background;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

// --- Recording and audio ------------------------------------------------------------------------

void MainComponent::toggle_record()
{
    auto& engine = context_.engine;
    const bool on = !engine.record_enabled();
    // Armed instrument tracks set to Print: their sound goes onto their locked
    // audio lanes as WAVs (with their effects), they play that, and their
    // instruments unload - the CPU and memory back. Unfreeze undoes it.
    if (on) {
        std::vector<std::string> printing;
        for (const auto& t : context_.session.data().tracks)
            if (t.record_armed && t.kind == project::TrackKind::Instrument && t.record_source == "print" && !t.instrument_unloaded)
                printing.push_back(t.id);
        if (!printing.empty() && context_.freeze_tracks) {
            context_.freeze_tracks(printing, true);
            for (const auto& id : printing) context_.session.set_record_armed(id, false);
            context_.status("Printing " + juce::String(static_cast<int>(printing.size())) +
                            (printing.size() == 1 ? " track" : " tracks") +
                            " to its audio lane: the instrument unloads when it is done (Unfreeze brings it back)");
            bool others = false;
            for (const auto& t : context_.session.data().tracks) others = others || t.record_armed;
            if (!others) {
                deck_.update_position();
                return;
            }
        }
    }
    engine.set_record_enabled(on);
    if (on) {
        int armed = 0;
        for (const auto& t : context_.session.data().tracks) armed += t.record_armed ? 1 : 0;
        context_.status(armed == 0 ? juce::String("Record enabled - arm a track (the red button in the track list) to record onto it")
                                   : "Record enabled - Play records onto " + juce::String(armed) +
                                         (armed == 1 ? " armed track" : " armed tracks"));
    } else {
        context_.status("Record off");
    }
    deck_.update_position();
    context_.commands.commandStatusChanged();
}

void MainComponent::set_loop(bool on)
{
    auto& session = context_.session;
    auto sel = session.selection();
    if (on && !sel.has_range()) {
        // No range yet: loop the bar the playhead is in.
        const auto& m = session.data().meter;
        const int bar = m.bar_at(context_.engine.position());
        syncing_range_ = false;
        session.set_selection_range(m.bar_start(bar), m.bar_start(bar + 1));
        sel = session.selection();
    }
    const bool changed = on != context_.engine.loop();
    context_.engine.set_loop(on, sel.from, sel.to);
    deck_.update_position();
    context_.commands.commandStatusChanged();
    if (changed) {
        if (on) {
            const auto& d = session.data();
            context_.status("Loop " + shell::bars_beats_ticks(d.meter, sel.from) + juce::String::fromUTF8(" \xe2\x86\x92 ") +
                            shell::bars_beats_ticks(d.meter, sel.to));
        } else {
            context_.status("Loop off");
        }
    }
}

void MainComponent::set_punch(bool on)
{
    const auto range = sequencer_.range();
    if (on && range.second <= range.first) {
        if (!punch_) context_.status("Drag across the sequencer ruler to choose the punch range first");
        on = false;
    }
    const bool changed = on != punch_;
    punch_ = on;
    context_.engine.set_punch(on, range.first, range.second);
    sequencer_.set_punch_display(on);
    toolbar_.update(on, sequencer_.pencil_mode(), static_cast<int>(sequencer_.edit_mode()), sequencer_.smart_tool());
    if (changed) {
        if (on) {
            const auto& m = context_.session.data().meter;
            auto where = [&](Ticks t) {
                const int bar = m.bar_at(t);
                const auto sig = m.at_bar(bar);
                return juce::String(bar) + "." + juce::String((t - m.bar_start(bar)) / sig.beat_ticks() + 1);
            };
            context_.status("Punch in at " + where(range.first) + ", out at " + where(range.second));
        } else if (range.second > range.first) {
            context_.status("Punch off");
        }
    }
    context_.commands.commandStatusChanged();
}

void MainComponent::import_audio()
{
    chooser_ = std::make_unique<juce::FileChooser>("Import audio or MIDI", juce::File(),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.mid;*.midi");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::canSelectMultipleItems,
                          [this](const juce::FileChooser& fc) {
                              const auto files = fc.getResults();
                              if (!files.isEmpty()) import_files(files, {}, context_.engine.position_seconds());
                          });
}

std::vector<std::string> MainComponent::import_audio_files(const juce::Array<juce::File>& files)
{
    const auto r = import_files(files, {}, context_.engine.position_seconds());
    return r.ids;
}

ImportResult MainComponent::import_files(const juce::Array<juce::File>& files, const std::string& track_id, double at_seconds)
{
    juce::Array<juce::File> audio, midi, scores;
    juce::File project;
    juce::StringArray skipped;
    for (const auto& f : files) {
        if (is_midi_file(f)) midi.add(f);
        else if (is_audio_file(f)) audio.add(f);
        else if (is_musicxml_file(f)) scores.add(f);
        else if (is_project_file(f) && project == juce::File()) project = f;
        else skipped.add(f.getFileName());
    }
    ImportResult all;
    // A project file: opened (after asking to save the one open), and that is all.
    if (project != juce::File()) {
        if (confirm_discard()) open_file(project);
        return all;
    }
    juce::StringArray messages;
    // MusicXML: each file's parts become tracks with their notation (one undo step a file).
    for (const auto& f : scores) {
        juce::String message;
        if (import_musicxml_file(context_, f, false, message)) messages.add(message);
        else all.unreadable.add(f.getFileName() + " (" + message + ")");
    }
    if (!audio.isEmpty()) {
        auto r = vcc::desktop::import_audio_files(context_, audio, track_id, at_seconds);
        all.ids = r.ids;
        all.tracks = r.tracks;
        all.unreadable.addArray(r.unreadable);
        if (r.message.isNotEmpty()) messages.add(r.message);
    }
    if (!midi.isEmpty()) {
        const Ticks at = context_.session.data().tempo.tick_at_seconds(std::max(0.0, at_seconds));
        auto r = import_midi_files(context_, midi, track_id, at);
        all.tracks.insert(all.tracks.end(), r.tracks.begin(), r.tracks.end());
        all.unreadable.addArray(r.unreadable);
        if (r.message.isNotEmpty()) messages.add(r.message);
    }
    all.unreadable.addArray(skipped);
    if (messages.isEmpty() && !all.unreadable.isEmpty()) {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions::makeOptionsOk(
                                         juce::MessageBoxIconType::WarningIcon, "Could not import",
                                         "These files could not be read as audio or MIDI:\n" + all.unreadable.joinIntoString("\n"), {},
                                         this),
                                     [](int) {});
        return all;
    }
    all.message = messages.joinIntoString("; ");
    if (!all.unreadable.isEmpty()) all.message << "; could not read " << all.unreadable.joinIntoString(", ");
    if (all.message.isNotEmpty()) context_.status(all.message);
    return all;
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files) {
        const juce::File file(f);
        if (is_audio_file(file) || is_midi_file(file) || is_musicxml_file(file) || is_project_file(file)) return true;
    }
    return false;
}

void MainComponent::filesDropped(const juce::StringArray& files, int, int)
{
    // Dropped anywhere but on a track: audio on a new track (or the selected
    // audio track) at the playhead; MIDI as new tracks at the playhead's bar.
    juce::Array<juce::File> list;
    for (const auto& f : files) list.add(juce::File(f));
    import_files(list, {}, context_.engine.position_seconds());
}

void MainComponent::new_audio_track()
{
    auto* w = new juce::AlertWindow("New Audio Track", "A track for recorded or imported audio", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", "Audio");
    w->addComboBox("format", {"Mono", "Stereo"}, "Input");
    w->getComboBoxComponent("format")->setSelectedItemIndex(0, juce::dontSendNotification);
    w->addButton("Add", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int r) {
        if (r != 1) return;
        const bool stereo = w->getComboBoxComponent("format")->getSelectedItemIndex() == 1;
        const juce::String name = w->getTextEditorContents("name").trim();
        context_.session.add_audio_track(name.isEmpty() ? "Audio" : name.toStdString(), stereo);
    }), true);
}

void MainComponent::new_bus(bool fx)
{
    auto* w = new juce::AlertWindow(fx ? "New Aux (FX) Bus" : "New Group Bus",
                                    fx ? "An effects return fed by track sends" : "A subgroup / stem that tracks can be routed to",
                                    juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", fx ? "Reverb" : "Group");
    w->addButton("Add", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w, fx](int r) {
        if (r != 1) return;
        const juce::String name = w->getTextEditorContents("name").trim();
        const std::string id = context_.session.add_bus(name.toStdString(), fx ? "fx" : "group");
        if (const auto* b = context_.session.data().find_bus(id))
            context_.status("Added " + juce::String(fx ? "aux bus " : "group bus ") + juce::String(b->name) + " - see the Mixer");
    }), true);
}

namespace {

/// Help > About VC Composer: the logo, the version and the credits.
class AboutPanel final : public juce::Component {
public:
    AboutPanel()
    {
        setComponentID("about_panel");
        setTitle("About VC Composer");
        text_.setText(juce::String("Version ") + Updater::current_version() +
                          (preview::is_preview() ? " (" + preview::status() + ")" : juce::String()) +
                          "\n\nA notation-first scoring workstation by VC Software.\n\n"
                          "Built with JUCE (commercial JUCE 8 licence).\n"
                          "Piano: Salamander Grand Piano by Alexander Holm (CC-BY 3.0).\n"
                          "Orchestra: MuseScore General by S. Christian Collins (MIT).\n"
                          "Everyone else who made VC Composer possible: Help > Credits.",
                      juce::dontSendNotification);
        text_.setJustificationType(juce::Justification::centredTop);
        text_.setColour(juce::Label::textColourId, colours::text);
        text_.setMinimumHorizontalScale(1.0f);
        addAndMakeVisible(text_);
        ok_.onClick = [this] {
            if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
        };
        addAndMakeVisible(ok_);
        setSize(520, 560);
    }
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff0a0908));  // the logo's own black
        const auto logo = brand_logo_stacked();
        if (logo.isValid()) {
            g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
            g.drawImageWithin(logo, 40, 20, getWidth() - 80, 300, juce::RectanglePlacement::centred);
        }
    }
    void resized() override
    {
        text_.setBounds(24, 330, getWidth() - 48, getHeight() - 330 - 56);
        ok_.setBounds(getWidth() / 2 - 50, getHeight() - 48, 100, 30);
    }

private:
    juce::Label text_;
    juce::TextButton ok_{"OK"};
};

}  // namespace

void MainComponent::show_about()
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(new AboutPanel());
    o.dialogTitle = "About VC Composer";
    o.dialogBackgroundColour = juce::Colour(0xff0a0908);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

// --- Commands ----------------------------------------------------------------------------------------

void MainComponent::getAllCommands(juce::Array<juce::CommandID>& c)
{
    c.addArray({cmd::newProjectWizard, cmd::newProject, cmd::newFromTemplate, cmd::open, cmd::save, cmd::saveAs, cmd::exportMidi,
                cmd::exportScoreSvg, cmd::importMusicXml, cmd::exportMusicXml, cmd::exportPartsPdf, cmd::exportFullScorePdf, cmd::exportStems, cmd::saveAsTemplate, cmd::undo, cmd::redo, cmd::play, cmd::rewind,
                cmd::toggleMetronome, cmd::toggleRecord, cmd::togglePunch, cmd::toggleLoop, cmd::addAudioTrack, cmd::importAudio,
                cmd::addAuxBus, cmd::addGroupBus, cmd::showComposer, cmd::showSequencer, cmd::showMixer, cmd::showVideo,
                cmd::showControlGallery, cmd::toggleView, cmd::toggleFollow, cmd::zoomIn, cmd::zoomOut,
                cmd::showCueManager, cmd::importPicture, cmd::addHit, cmd::chordExplorer,
                cmd::ostinatoCreator, cmd::addTemplate, cmd::addTrack, cmd::duplicateVersion, cmd::removeTrack,
                cmd::audioSettings, cmd::scanPlugins, cmd::loadInstrument, cmd::openInstrumentWindow, cmd::reloadCrashedPlugins,
                cmd::about, cmd::editShuffle, cmd::editSlip, cmd::editSpot, cmd::editGrid, cmd::toggleSmartTool,
                cmd::showCircleOfFifths, cmd::toggleStaffTools, cmd::detachStaffTools, cmd::showRouting,
                cmd::showControlSurfaces, cmd::jogForward, cmd::jogBack, cmd::showReview,
                cmd::collectPackage, cmd::openPackage, cmd::showComments, cmd::addComment, cmd::relinkMedia,
                cmd::articulationMap, cmd::missingInstruments, cmd::mixTools, cmd::bounceInPlace, cmd::saveOnStop,
                cmd::monitorDim, cmd::reverseClips, cmd::timeStretchClips, cmd::groupClips, cmd::ungroupClips,
                cmd::captureMidi, cmd::midiInputFilter, cmd::lyrics, cmd::explodeTracks, cmd::implodeTracks, cmd::spreadChords, cmd::addRepeat,
                cmd::unrollRepeats, cmd::editShortcuts, cmd::printPreview, cmd::exportPicture, cmd::tempoFinder, cmd::account, cmd::freezeTrack, cmd::freezeUnload,
                cmd::unfreezeTrack, cmd::rerenderTrack, cmd::expandArticulations, cmd::consolidateArticulations,
                cmd::instrumentUsage, cmd::sharedProjects, cmd::shareProject, cmd::syncShared, cmd::lockPart, cmd::unlockPart,
                cmd::checkUpdates, cmd::toggleDock, cmd::toggleWorkView, cmd::saveLayout, cmd::midiSetup, cmd::mixerController, cmd::scoreView, cmd::trackList, cmd::hearEntry,
                cmd::builtinSounds, cmd::credits, cmd::toggleInspector, cmd::libraryWindow, cmd::soundAssignments, cmd::scoreInfo});
}

void MainComponent::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    using KP = juce::KeyPress;
    const auto cmdKey = juce::ModifierKeys::commandModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;
    const bool has_track = !context_.session.selected_track().empty();
    switch (id) {
        case cmd::newProject: info.setInfo("New Empty Project", "", "File", 0); info.addDefaultKeypress('n', cmdKey | shift); break;
        case cmd::newProjectWizard:
            info.setInfo("New Project...", "Name, template, key, meter, tempo, frame rate and picture", "File", 0);
            info.addDefaultKeypress('n', cmdKey);
            break;
        case cmd::newFromTemplate: info.setInfo("New from Template...", "", "File", 0); break;
        case cmd::open: info.setInfo("Open...", "", "File", 0); info.addDefaultKeypress('o', cmdKey); break;
        case cmd::save: info.setInfo("Save", "", "File", 0); info.addDefaultKeypress('s', cmdKey); break;
        case cmd::saveAs: info.setInfo("Save As...", "", "File", 0); info.addDefaultKeypress('s', cmdKey | shift); break;
        case cmd::exportMidi: info.setInfo("Export MIDI...", "", "File", 0); info.addDefaultKeypress('e', cmdKey); break;
        case cmd::exportScoreSvg: info.setInfo("Export Parts (SVG pages)...", "", "File", 0); break;
        case cmd::importMusicXml:
            info.setInfo("Import MusicXML...", "Adds a MusicXML or .mxl score's parts as tracks", "File", 0);
            break;
        case cmd::exportMusicXml:
            info.setInfo("Export MusicXML...", "For Sibelius, Finale, Dorico, MuseScore and engravers", "File", 0);
            break;
        case cmd::exportFullScorePdf:
            info.setInfo("Export Full Score (PDF)...", "Every part on one system, transposed or in C", "File", 0);
            break;
        case cmd::exportStems:
            info.setInfo("Export Stems...", "The mix and each stem as WAV, for the project or every cue", "File", 0);
            info.addDefaultKeypress('e', cmdKey | juce::ModifierKeys::shiftModifier);
            break;
        case cmd::exportPartsPdf:
            info.setInfo("Export Parts (PDF)...", "Printed parts, one PDF each or all in one", "File", 0);
            info.addDefaultKeypress('p', cmdKey | juce::ModifierKeys::shiftModifier);
            break;
        case cmd::saveAsTemplate: info.setInfo("Save as Template...", "", "File", 0); break;
        case cmd::undo:
            info.setInfo(context_.session.can_undo() ? "Undo " + juce::String(context_.session.undo_label()) : juce::String("Undo"),
                         "", "Edit", context_.session.can_undo() ? 0 : juce::ApplicationCommandInfo::isDisabled);
            info.addDefaultKeypress('z', cmdKey);
            break;
        case cmd::redo:
            info.setInfo("Redo", "", "Edit", context_.session.can_redo() ? 0 : juce::ApplicationCommandInfo::isDisabled);
            info.addDefaultKeypress('z', cmdKey | shift);
            break;
        case cmd::play: info.setInfo(context_.engine.playing() ? "Stop" : "Play", "", "Transport", 0); info.addDefaultKeypress(KP::spaceKey, 0); break;
        case cmd::rewind:
            info.setInfo("Return to Start", "", "Transport", 0);
            info.addDefaultKeypress(KP::homeKey, 0);
            info.addDefaultKeypress(KP::returnKey, 0);  // Enter (docs/spec/ui shortcut table)
            break;
        case cmd::toggleMetronome:
            info.setInfo("Metronome", "", "Transport", context_.engine.metronome() ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('k', cmdKey);
            break;
        case cmd::toggleRecord:
            // R, as in most DAWs; in the Composer's note entry R is the rest key and wins there.
            info.setInfo("Record", "Record enable", "Transport", context_.engine.record_enabled() ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('r', 0);
            break;
        case cmd::toggleLoop:
            info.setInfo("Loop Playback", "Play the selected range over and over", "Transport",
                         context_.engine.loop() ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('l', 0);
            break;
        case cmd::togglePunch:
            info.setInfo("Punch In/Out", "Record only inside the sequencer ruler range", "Transport",
                         punch_ ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('p', cmdKey | shift);
            break;
        case cmd::addAudioTrack: info.setInfo("New Audio Track...", "", "Track", 0); info.addDefaultKeypress('t', cmdKey | shift); break;
        case cmd::importAudio: info.setInfo("Import Audio or MIDI...", "", "File", 0); info.addDefaultKeypress('i', cmdKey | juce::ModifierKeys::altModifier); break;
        case cmd::addAuxBus: info.setInfo("New Aux (FX) Bus...", "", "Track", 0); break;
        case cmd::addGroupBus: info.setInfo("New Group Bus...", "", "Track", 0); break;
        case cmd::showComposer:
            info.setInfo("Composer View", "", "View", workspace_ == Workspace::Composer ? juce::ApplicationCommandInfo::isTicked : 0);
            break;
        case cmd::showSequencer:
            info.setInfo("Producer View", "", "View", workspace_ == Workspace::Sequencer ? juce::ApplicationCommandInfo::isTicked : 0);
            break;
        case cmd::showMixer: info.setInfo("Mixer", "", "View", 0); info.addDefaultKeypress('m', cmdKey); break;
        case cmd::mixTools: info.setInfo("Groups, VCAs and Monitor...", "VCA faders, mix groups, monitor dim/mono, cue mixes", "Track", 0); break;
        case cmd::bounceInPlace:
            info.setInfo("Bounce in Place", "Renders the selected track (with its strip and inserts) onto itself", "Track",
                         has_track ? 0 : juce::ApplicationCommandInfo::isDisabled);
            break;
        case cmd::saveOnStop:
            info.setInfo("Save on Transport Stop", "Saves the project whenever playback stops", "File",
                         hw::ui_settings().getBoolValue("save_on_stop", false) ? juce::ApplicationCommandInfo::isTicked : 0);
            break;
        case cmd::reverseClips:
            info.setInfo("Reverse Clips", "The selected clips play backwards (new files; originals kept)", "Edit", 0);
            break;
        case cmd::timeStretchClips:
            info.setInfo("Time Stretch Clips...", "Longer or shorter, pitch kept (new files; originals kept)", "Edit", 0);
            break;
        case cmd::groupClips:
            info.setInfo("Group Clips", "The selected clips move together", "Edit", 0);
            info.addDefaultKeypress('g', cmdKey);
            break;
        case cmd::ungroupClips:
            info.setInfo("Ungroup Clips", "", "Edit", 0);
            info.addDefaultKeypress('g', cmdKey | shift);
            break;
        case cmd::captureMidi:
            info.setInfo("Capture MIDI", "What you just played on a MIDI keyboard, as a region on the selected track",
                         "Transport", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled);
            info.addDefaultKeypress('r', cmdKey | shift);
            break;
        case cmd::midiInputFilter: info.setInfo("MIDI Input Filter...", "Channels, notes and messages let through", "Tools", 0); break;
        case cmd::lyrics: info.setInfo("Lyrics...", "Syllables on the selected notes", "Edit", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled); info.addDefaultKeypress('l', cmdKey | shift); break;
        case cmd::spreadChords:
            info.setInfo("Spread Chords to Strings", "The selected track's chords (the piano) as open five-part chords: violins I and II, violas, celli, basses",
                         "Edit", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled);
            info.addDefaultKeypress('k', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier);
            break;
        case cmd::explodeTracks: info.setInfo("Explode...", "Chords split into new tracks", "Edit", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled); break;
        case cmd::implodeTracks: info.setInfo("Implode...", "Tracks combined as chords onto the selected one", "Edit", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled); break;
        case cmd::addRepeat: info.setInfo("Repeat and Endings...", "Repeat bars, with first and second endings", "Edit", 0); break;
        case cmd::unrollRepeats: info.setInfo("Unroll Repeats", "Writes the repeats out into the bars", "Edit", 0); break;
        case cmd::editShortcuts: info.setInfo("Edit Shortcuts...", "Keys for every command; your own profiles", "Edit", 0); break;
        case cmd::exportPicture:
            info.setInfo("Export Picture with Score...", "The cut with the score mix, timecode and watermark, for director review",
                         "File", context_.session.data().active_picture() ? 0 : juce::ApplicationCommandInfo::isDisabled);
            break;
        case cmd::tempoFinder:
            info.setInfo("Tempo Finder...", "Steady tempos that put the hit points on the beat", "Transport", 0);
            break;
        case cmd::printPreview:
            info.setInfo("Print Preview...", "The full score or a part as it will print", "File", 0);
            info.addDefaultKeypress('p', cmdKey | shift);
            break;
        case cmd::monitorDim:
            info.setInfo("Dim Monitor", "Lowers what the speakers play (never an export)", "Transport",
                         context_.engine.monitor().dim ? juce::ApplicationCommandInfo::isTicked : 0);
            break;
        case cmd::showControlGallery: info.setInfo("Control Gallery", "", "View", 0); break;
        case cmd::showReview:
            info.setInfo("Director Review...", "A/B/C: compare snapshots or a track's versions, approve one", "View", 0);
            break;
        case cmd::collectPackage:
            info.setInfo("Collect and Save Package...", "The project and every media file it uses, in one folder", "File", 0);
            break;
        case cmd::openPackage: info.setInfo("Open Package...", "Unpacks a package .zip and opens it", "File", 0); break;
        case cmd::relinkMedia:
            info.setInfo("Relink Missing Media...", "Finds moved audio and picture files and points the project at them",
                         "File", 0);
            break;
        case cmd::showComments: info.setInfo("Comments...", "Review comments pinned to moments", "View", 0); break;
        case cmd::addComment:
            info.setInfo("Add Comment at Playhead...", "Pins a review comment where the playhead is", "View", 0);
            break;
        case cmd::showControlSurfaces:
            info.setInfo("Control Surfaces...", "Jog/shuttle wheels and transport controllers", "Tools", 0);
            break;
        case cmd::jogForward: info.setInfo("Jog Forward", "One jog step forward (for devices that type keys)", "Transport", 0); break;
        case cmd::jogBack: info.setInfo("Jog Back", "One jog step back (for devices that type keys)", "Transport", 0); break;
        case cmd::showRouting:
            info.setInfo("Visual Routing...", "Tracks, buses, sends and outputs as a signal-flow map", "View", 0);
            info.addDefaultKeypress('r', cmdKey | juce::ModifierKeys::altModifier);
            break;
        case cmd::showCircleOfFifths:
            info.setInfo("Circle of Fifths...", "Key reference: scales, diatonic chords, related keys", "View", 0);
            break;
        case cmd::toggleStaffTools:
            info.setInfo("Staff Tools", "Show or fold the tools beside the selected staff", "View",
                         composer_.staff_tools_collapsed() ? 0 : juce::ApplicationCommandInfo::isTicked);
            info.addDefaultKeypress('t', cmdKey | juce::ModifierKeys::altModifier);
            break;
        case cmd::detachStaffTools:
            info.setInfo("Detach Staff Tools", "Float the staff tools in their own window", "View",
                         composer_.staff_tools_detached() ? juce::ApplicationCommandInfo::isTicked : 0);
            break;
        case cmd::editShuffle:
        case cmd::editSlip:
        case cmd::editSpot:
        case cmd::editGrid: {
            static const char* names[] = {"Shuffle", "Slip", "Spot", "Grid"};
            const int i = static_cast<int>(id - cmd::editShuffle);
            info.setInfo(juce::String(names[i]) + " Mode", "Producer view edit mode", "Edit",
                         static_cast<int>(sequencer_.edit_mode()) == i ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress(KP::F1Key + i, 0);
            break;
        }
        case cmd::toggleSmartTool:
            info.setInfo("Smart Tool", "On a clip: the top half selects a range, the bottom half moves it", "Edit",
                         sequencer_.smart_tool() ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress(KP::F6Key, 0);
            break;
        case cmd::zoomIn: info.setInfo("Zoom In", "", "View", 0); info.addDefaultKeypress('=', cmdKey); break;
        case cmd::zoomOut: info.setInfo("Zoom Out", "", "View", 0); info.addDefaultKeypress('-', cmdKey); break;
        case cmd::toggleView:
            info.setInfo("Switch Composer / Producer", "", "View", 0);
            info.addDefaultKeypress(juce::KeyPress::tabKey, juce::ModifierKeys::ctrlModifier);
            info.addDefaultKeypress('`', juce::ModifierKeys::commandModifier);
            break;
        case cmd::toggleFollow:
            info.setInfo("Follow Playhead", "The edit window scrolls with the playhead while it plays", "View",
                         context_.follow ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('f', cmdKey | shift);
            break;
        case cmd::showVideo: info.setInfo("Picture", "", "View", 0); info.addDefaultKeypress('p', cmdKey | juce::ModifierKeys::altModifier); break;
        case cmd::showCueManager: info.setInfo("Cue Manager", "", "View", 0); info.addDefaultKeypress('u', cmdKey | juce::ModifierKeys::altModifier); break;
        case cmd::importPicture: info.setInfo("Import Picture...", "", "File", 0); info.addDefaultKeypress('i', cmdKey | shift); break;
        case cmd::addHit: info.setInfo("Mark Hit at Playhead", "", "Transport", 0); info.addDefaultKeypress('h', cmdKey | shift); break;
        case cmd::chordExplorer: info.setInfo("Chord Explorer...", "", "Tools", 0); info.addDefaultKeypress('h', cmdKey | shift); break;
        case cmd::ostinatoCreator: info.setInfo("Ostinato Creator...", "", "Tools", 0); info.addDefaultKeypress('o', cmdKey | shift); break;
        case cmd::addTemplate: info.setInfo("Add Template to Project...", "", "Templates", 0); break;
        case cmd::addTrack: info.setInfo("New Track...", "", "Track", 0); info.addDefaultKeypress('t', cmdKey); break;
        case cmd::duplicateVersion: info.setInfo("Duplicate as New Version", "", "Track", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled); info.addDefaultKeypress('d', cmdKey | shift); break;
        case cmd::removeTrack: info.setInfo("Remove Track", "", "Track", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled); break;
        case cmd::loadInstrument: info.setInfo("Instrument...", "", "Track", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled); break;
        case cmd::openInstrumentWindow: info.setInfo("Open Instrument Window", "", "Track", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled); break;
        case cmd::articulationMap:
            info.setInfo("Articulation Map...", "How written articulations reach the instrument: keyswitches, controllers...",
                         "Track", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled);
            break;
        case cmd::missingInstruments:
            info.setInfo("Missing Instruments...", "Plug-ins this project uses that are not installed here", "Track", 0);
            break;
        case cmd::audioSettings:
            info.setInfo("Audio Setup...", "The interface, sample rate, buffer size, inputs and outputs", "Studio", 0);
            info.addDefaultKeypress(',', cmdKey);
            break;
        case cmd::midiSetup: info.setInfo("MIDI Setup...", "MIDI keyboards and controllers (USB, USB-C), on or off", "Studio", 0); break;
        case cmd::builtinSounds:
            info.setInfo("Built-in Sounds...", "The piano and General MIDI orchestra: download, status, credits", "Studio", 0);
            break;
        case cmd::scoreInfo:
            info.setInfo("Sheet Info...", "The score's title (styled), composer, date, version and the other credits, and which of them print", "File", 0);
            break;
        case cmd::soundAssignments:
            info.setInfo("Sound Assignments...", "Your sound for each instrument and articulation, used in every template and new track",
                         "Window", 0);
            break;
        case cmd::libraryWindow:
            info.setInfo("Library", "Sounds, libraries and players: search, audition, assign - in a floating window", "Window", 0);
            info.setTicked(library_shown());
            info.addDefaultKeypress('l', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier);
            break;
        case cmd::toggleInspector:
            info.setInfo("Inspector", "What is selected, with its articulation, dynamics, velocity and voice keys, in a floating window",
                         "Window", 0);
            info.setTicked(inspector_shown());
            break;
        case cmd::credits:
            info.setInfo("Credits...", "The open-source sounds, software and fonts in VC Composer, and trademark notices", "Help", 0);
            break;
        case cmd::mixerController:
            info.setInfo("Mixer Controller (Mackie Control)...",
                         "A hardware mixer controller: SSL UF8, Avid S1, Icon, X-Touch, FaderPort, MCU", "Studio", 0);
            break;
        case cmd::scanPlugins: info.setInfo("Plug-ins...", "", "Tools", 0); break;
        case cmd::reloadCrashedPlugins:
            info.setInfo("Reload Crashed Plug-ins", "Starts crashed plug-ins again from their last state", "Tools", 0);
            break;
        case cmd::freezeTrack:
            info.setInfo("Freeze", "Print the selected track through its inserts and play the print; the instrument stays loaded",
                         "Track", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled);
            break;
        case cmd::freezeUnload:
            info.setInfo("Freeze && Unload", "Print the selected track, play the print, and close its instrument to free memory",
                         "Track", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled);
            break;
        case cmd::unfreezeTrack: {
            const auto* t = context_.session.data().find_track(context_.session.selected_track());
            info.setInfo("Unfreeze", "Back to the live instrument; the print stays on the audio lane", "Track",
                         t && (t->playback == "audio" || t->instrument_unloaded) ? 0 : juce::ApplicationCommandInfo::isDisabled);
            break;
        }
        case cmd::rerenderTrack: {
            const auto* t = context_.session.data().find_track(context_.session.selected_track());
            info.setInfo("Re-render", "Print the selected track again (a stale print: the music or instrument changed)", "Track",
                         t && !t->print_signature.empty() ? 0 : juce::ApplicationCommandInfo::isDisabled);
            break;
        }
        case cmd::sharedProjects:
            info.setInfo("Shared Projects...", "Projects shared with you, and this project's sharing: sync, people, locks", "File", 0);
            break;
        case cmd::shareProject:
            info.setInfo("Share Project...", "Put this project on vc-composer.com so the people you invite can open and sync it", "File",
                         !context_.session.path().empty() && !collab::Controller::get().link()
                             ? 0
                             : juce::ApplicationCommandInfo::isDisabled);
            break;
        case cmd::syncShared:
            info.setInfo("Sync Shared Project", "Bring in what the others pushed, then push your changes", "File",
                         collab::Controller::get().link() && !collab::Controller::get().busy() ? 0
                                                                                             : juce::ApplicationCommandInfo::isDisabled);
            info.addDefaultKeypress('y', cmdKey | juce::ModifierKeys::shiftModifier);
            break;
        case cmd::lockPart:
        case cmd::unlockPart:
            info.setInfo(id == cmd::lockPart ? "Lock Part for Writing" : "Unlock Part",
                         id == cmd::lockPart ? "Only you can change the selected part until you unlock it"
                                             : "Others can write the selected part again",
                         "Track",
                         collab::Controller::get().link() && !context_.session.selected_track().empty()
                             ? 0
                             : juce::ApplicationCommandInfo::isDisabled);
            break;
        case cmd::instrumentUsage:
            info.setInfo("Instrument Usage...", "Every instrument's memory and engine load, with Freeze & Unload, Suspend and Reload",
                         "Tools", 0);
            break;
        case cmd::expandArticulations: {
            const auto* t = context_.session.data().find_track(articulation_part(context_, context_.session.selected_track()));
            info.setInfo("Expand Articulations to Tracks...",
                         "A track per sound the part plays (staccato, pizzicato...), each with its own instrument; "
                         "the notes stay in the part's score",
                         "Track",
                         t && t->kind == project::TrackKind::Instrument && !t->articulations_expanded
                             ? 0
                             : juce::ApplicationCommandInfo::isDisabled);
            break;
        }
        case cmd::consolidateArticulations: {
            const auto* t = context_.session.data().find_track(articulation_part(context_, context_.session.selected_track()));
            info.setInfo("Consolidate to One Track", "The articulation tracks go and the part plays every sound itself again",
                         "Track", t && t->articulations_expanded ? 0 : juce::ApplicationCommandInfo::isDisabled);
            break;
        }
        case cmd::account: info.setInfo("Account and Licence...", "Sign in, activate this computer, see the licence", "Help", 0); break;
        case cmd::about: info.setInfo("About VC Composer", "", "Help", 0); break;
        case cmd::checkUpdates:
            info.setInfo(update_waiting_.isNotEmpty() ? "Install Update " + update_waiting_ + "..." : juce::String("Check for Updates..."),
                         "Whether vc-composer.com has a newer VC Composer, and install it", "Help",
                         Updater::available() ? 0 : juce::ApplicationCommandInfo::isDisabled);
            break;
        case cmd::toggleDock:
            info.setInfo(dock_hidden_ ? "Show Keyboard / Mixer Deck" : "Hide Keyboard / Mixer Deck",
                         "The bottom deck: the keyboard, or the mixer in Producer", "View", 0);
            info.addDefaultKeypress('b', cmdKey | juce::ModifierKeys::altModifier);
            break;
        case cmd::toggleWorkView:
            info.setInfo("Work View", "Hides the side panels: the edit window as large as it gets", "View",
                         work_view_ ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('w', cmdKey | juce::ModifierKeys::altModifier);
            break;
        case cmd::saveLayout: info.setInfo("Save Layout...", "Saves how the window is arranged now, by name", "View", 0); break;
        case cmd::scoreView:
            info.setInfo("Score View", "The full score on pages, laid out as it prints and exports", "View",
                         workspace_ == Workspace::Composer && page_mode_ ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('s', cmdKey | juce::ModifierKeys::altModifier);
            break;
        case cmd::hearEntry:
            info.setInfo("Hear Notes as Placed", "Each note sounds as it is placed", "Edit",
                         composer_.hear_entry() ? juce::ApplicationCommandInfo::isTicked : 0);
            break;
        case cmd::trackList:
            info.setInfo("Track List", "A floating list of the tracks: click one to go to it", "View",
                         track_list_shown() ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('l', cmdKey | juce::ModifierKeys::altModifier);
            break;
        default: break;
    }
}

bool MainComponent::perform(const InvocationInfo& info)
{
    auto& s = context_.session;
    switch (info.commandID) {
        case cmd::newProject:
            if (confirm_discard()) s.new_empty();
            break;
        case cmd::newFromTemplate:
            if (confirm_discard()) show_template_browser(context_, true);
            break;
        case cmd::newProjectWizard:
            if (confirm_discard()) show_new_project_wizard(context_);
            break;
        case cmd::open:
            if (!confirm_discard()) break;
            chooser_ = std::make_unique<juce::FileChooser>("Open project", project_folder_for_dialogs(), "*.vcc;*.vcc2");
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      if (fc.getResult() != juce::File()) open_file(fc.getResult());
                                  });
            break;
        case cmd::save: save(false); break;
        case cmd::saveAs: save(true); break;
        case cmd::exportMidi: export_midi(); break;
        case cmd::exportScoreSvg:
            chooser_ = std::make_unique<juce::FileChooser>("Choose a folder for the parts", juce::File());
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [this](const juce::FileChooser& fc) {
                                      if (fc.getResult().isDirectory()) export_parts_svg(context_, fc.getResult());
                                  });
            break;
        case cmd::importMusicXml: import_musicxml(); break;
        case cmd::exportMusicXml: export_musicxml(); break;
        case cmd::exportPartsPdf: export_parts_pdf_dialog(); break;
        case cmd::exportFullScorePdf: export_full_score_dialog(); break;
        case cmd::exportStems: show_stem_export(context_); break;
        case cmd::saveAsTemplate: save_as_template(); break;
        case cmd::undo: s.undo(); break;
        case cmd::redo: s.redo(); break;
        case cmd::play: context_.engine.toggle_play(); break;
        case cmd::rewind: context_.go_to_start(); break;
        case cmd::toggleMetronome: context_.engine.set_metronome(!context_.engine.metronome()); break;
        case cmd::toggleRecord: toggle_record(); break;
        case cmd::togglePunch: set_punch(!punch_); break;
        case cmd::toggleLoop: set_loop(!context_.engine.loop()); break;
        case cmd::addAudioTrack: new_audio_track(); break;
        case cmd::importAudio: import_audio(); break;
        case cmd::addAuxBus: new_bus(true); break;
        case cmd::addGroupBus: new_bus(false); break;
        case cmd::showComposer: show_workspace(Workspace::Composer); break;
        case cmd::showSequencer: show_workspace(Workspace::Sequencer); break;
        case cmd::showMixer: show_mixer(); break;
        case cmd::showControlGallery: show_control_gallery(); break;
        case cmd::showCircleOfFifths: composer_ui::show_circle_of_fifths(context_); break;
        case cmd::showRouting: console::show_routing(context_); break;
        case cmd::showControlSurfaces: surface_ui::show_control_surfaces(context_, *surface_); break;
        case cmd::showReview: review::show_review(context_); break;
        case cmd::showComments: review::show_comments(context_); break;
        case cmd::addComment:
            review::show_comments(context_);
            if (auto* p = review::comments_panel()) p->entry().grabKeyboardFocus();
            break;
        case cmd::collectPackage: review::show_package(context_); break;
        case cmd::relinkMedia: review::show_relink(context_); break;
        case cmd::mixTools: show_mix_tools(context_); break;
        case cmd::reverseClips: context_.status(reverse_clips(context_, sequencer_.selected_clips())); break;
        case cmd::timeStretchClips: ask_time_stretch(context_, sequencer_.selected_clips()); break;
        case cmd::groupClips: {
            const auto clips = sequencer_.selected_clips();
            context_.status(clips.size() < 2 ? juce::String("Select two or more clips to group")
                            : !s.group_items(clips).empty() ? "Grouped " + juce::String(static_cast<int>(clips.size())) + " clips: they move together"
                                                            : juce::String("Could not group them"));
            break;
        }
        case cmd::ungroupClips:
            context_.status(s.ungroup_items(sequencer_.selected_clips()) ? juce::String("Ungrouped") : juce::String("No group selected"));
            break;
        case cmd::captureMidi: context_.status(capture_midi(context_, s.selected_track())); break;
        case cmd::midiInputFilter: show_midi_input_filter(context_); break;
        case cmd::lyrics: ask_lyrics(context_); break;
        case cmd::explodeTracks: ask_explode(context_); break;
        case cmd::spreadChords: context_.status(spread_to_strings(context_, s.selected_track())); break;
        case cmd::implodeTracks: ask_implode(context_); break;
        case cmd::addRepeat: ask_repeat(context_); break;
        case cmd::unrollRepeats: context_.status(unroll(context_)); break;
        case cmd::printPreview: show_print_preview(context_); break;
        case cmd::exportPicture: show_picture_export(context_); break;
        case cmd::tempoFinder: show_tempo_finder(context_); break;
        case cmd::account: show_account_window(context_); break;
        case cmd::freezeTrack:
        case cmd::freezeUnload:
            if (!s.selected_track().empty()) context_.freeze_tracks({s.selected_track()}, info.commandID == cmd::freezeUnload);
            break;
        case cmd::unfreezeTrack:
            if (s.unfreeze(s.selected_track())) context_.status("Unfrozen: the live instrument plays again");
            break;
        case cmd::rerenderTrack:
            if (const auto* t = s.data().find_track(s.selected_track()))
                context_.freeze_tracks({t->id}, t->instrument_unloaded);
            break;
        case cmd::expandArticulations: ask_expand_articulations(context_); break;
        case cmd::instrumentUsage: show_instrument_usage(context_); break;
        case cmd::sharedProjects: show_shared_projects(context_, open_shared_file()); break;
        case cmd::shareProject:
            show_shared_projects(context_, open_shared_file());
            break;
        case cmd::syncShared:
            if (may_save(context_))
                collab::Controller::get().sync([](collab::Outcome) {});
            break;
        case cmd::lockPart:
        case cmd::unlockPart:
            collab::Controller::get().set_lock(s.selected_track(), info.commandID == cmd::lockPart, [this](collab::Outcome o) {
                context_.status(o.message);
            });
            break;
        case cmd::consolidateArticulations: context_.status(consolidate_articulations(context_)); break;
        case cmd::editShortcuts:
            show_shortcut_editor(context_, [this](const std::string& name) { use_shortcut_profile(name); });
            break;
        case cmd::bounceInPlace: context_.status(bounce_in_place(context_, s.selected_track())); break;
        case cmd::saveOnStop: {
            auto& ui = hw::ui_settings();
            ui.setValue("save_on_stop", !ui.getBoolValue("save_on_stop", false));
            ui.saveIfNeeded();
            context_.commands.commandStatusChanged();
            break;
        }
        case cmd::monitorDim: {
            auto m = context_.engine.monitor();
            m.dim = !m.dim;
            context_.engine.set_monitor(m);
            context_.status(m.dim ? "Monitor dimmed " + juce::String(m.dim_db, 0) + " dB" : juce::String("Monitor at full level"));
            context_.commands.commandStatusChanged();
            break;
        }
        case cmd::articulationMap:
            if (!s.selected_track().empty()) show_articulation_map(context_, s.selected_track());
            break;
        case cmd::missingInstruments:
            if (show_missing_instruments(context_) == 0) context_.status("Every instrument plug-in this project uses is installed");
            break;
        case cmd::openPackage:
            if (!confirm_discard()) break;
            chooser_ = std::make_unique<juce::FileChooser>("Open package", juce::File(), "*.zip");
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      if (fc.getResult() != juce::File()) open_package(fc.getResult());
                                  });
            break;
        case cmd::jogForward: surface_->jog(1); break;
        case cmd::jogBack: surface_->jog(-1); break;
        case cmd::toggleStaffTools:
            composer_.set_staff_tools_collapsed(!composer_.staff_tools_collapsed());
            break;
        case cmd::detachStaffTools: composer_.set_staff_tools_detached(!composer_.staff_tools_detached()); break;
        case cmd::editShuffle:
        case cmd::editSlip:
        case cmd::editSpot:
        case cmd::editGrid:
            sequencer_.set_edit_mode(static_cast<SequencerView::EditMode>(info.commandID - cmd::editShuffle));
            refresh_tool_keys();
            break;
        case cmd::toggleSmartTool:
            sequencer_.set_smart_tool(!sequencer_.smart_tool());
            refresh_tool_keys();
            break;
        case cmd::toggleView:
            show_workspace(workspace_ == Workspace::Composer ? Workspace::Producer : Workspace::Composer);
            break;
        case cmd::toggleFollow: set_follow(!context_.follow); break;
        case cmd::zoomIn:
        case cmd::zoomOut:
            if (workspace_ == Workspace::Composer) {
                if (page_mode_) page_view_.zoom(info.commandID == cmd::zoomIn);
                else composer_.zoom(info.commandID == cmd::zoomIn);
            }
            else sequencer_.zoom(info.commandID == cmd::zoomIn);
            break;
        case cmd::showVideo: show_video_window(context_); break;
        case cmd::showCueManager: show_cue_manager(context_); break;
        case cmd::importPicture:
            chooser_ = std::make_unique<juce::FileChooser>("Import picture", juce::File(), "*.mov;*.mp4;*.m4v;*.mxf;*.avi;*.mkv");
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      if (fc.getResult() == juce::File()) return;
                                      juce::String error;
                                      if (import_picture_file(context_, fc.getResult(), error)) show_video_window(context_);
                                      else juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                                  "Could not import picture", error);
                                  });
            break;
        case cmd::addHit: {
            const double now = context_.engine.position_seconds();
            s.add_marker(now, "hit");
            context_.status("Hit at " + juce::String(picture::project_timecode(s.data(), now)));
            break;
        }
        case cmd::chordExplorer: show_chord_explorer(context_); break;
        case cmd::ostinatoCreator: show_ostinato_creator(context_); break;
        case cmd::addTemplate: show_template_browser(context_, false); break;
        case cmd::addTrack: TrackPanel::show_add_track_menu(context_, &tracks_); break;
        case cmd::duplicateVersion:
            if (!s.selected_track().empty()) s.duplicate_version(s.selected_track());
            break;
        case cmd::removeTrack:
            if (!s.selected_track().empty()) s.remove_track(s.selected_track());
            break;
        case cmd::loadInstrument:
            if (!s.selected_track().empty()) TrackPanel::show_track_menu(context_, s.selected_track());
            break;
        case cmd::openInstrumentWindow:
            if (!s.selected_track().empty()) show_plugin_window(s.selected_track());
            break;
        case cmd::audioSettings: show_studio_setup(context_, 0); break;
        case cmd::midiSetup: show_studio_setup(context_, 1); break;
        case cmd::mixerController: show_mixer_controller_window(context_, *mixer_controller_); break;
        case cmd::builtinSounds: show_sounds_window(); break;
        case cmd::credits: show_credits_window(); break;
        case cmd::toggleInspector: set_inspector_shown(!inspector_shown()); break;
        case cmd::libraryWindow: set_library_shown(!library_shown()); break;
        case cmd::soundAssignments: show_sound_assignments(context_); break;
        case cmd::scoreInfo: show_score_info(context_); break;
        case cmd::scanPlugins: show_plugin_scanner(); break;
        case cmd::reloadCrashedPlugins: {
            const int n = context_.engine.reload_crashed_plugins();
            context_.status(n == 0 ? juce::String("No plug-in has crashed")
                                   : "Reloaded " + juce::String(n) + " crashed plug-in" + (n == 1 ? "" : "s"));
            break;
        }
        case cmd::about: show_about(); break;
        case cmd::checkUpdates: show_update_window(context_, [this] { return confirm_discard(); }); break;
        case cmd::toggleDock: set_dock_hidden(!dock_hidden_); break;
        case cmd::toggleWorkView: set_work_view(!work_view_); break;
        case cmd::saveLayout: save_layout_as(); break;
        case cmd::scoreView:
            if (workspace_ == Workspace::Composer && page_mode_) set_page_view(false);
            else set_page_view(true);
            break;
        case cmd::trackList: toggle_track_list(); break;
        case cmd::hearEntry:
            composer_.set_hear_entry(!composer_.hear_entry());
            refresh_tool_keys();
            context_.status(composer_.hear_entry() ? "Notes sound as they are placed" : "Notes are placed silently");
            break;
        default: return false;
    }
    return true;
}

// --- Menus -------------------------------------------------------------------------------------

juce::StringArray MainComponent::getMenuBarNames()
{
    return {"File", "Edit", "View", "Track", "Templates", "Transport", "Tools", "Studio", "Window", "Help"};
}

juce::PopupMenu MainComponent::getMenuForIndex(int, const juce::String& name)
{
    juce::PopupMenu m;
    auto* c = &context_.commands;
    const int index = juce::StringArray{"File", "Edit", "View", "Transport", "Track", "Tools", "Help", "Templates", "Window", "Studio"}.indexOf(name);
    switch (index) {
        case 0:
            for (auto id : {cmd::newProjectWizard, cmd::newFromTemplate, cmd::newProject, cmd::open})
                m.addCommandItem(c, id);
            m.addSubMenu("Open Recent", recent_menu());
            m.addCommandItem(c, cmd::openPackage);
            m.addSeparator();
            for (auto id : {cmd::sharedProjects, cmd::shareProject, cmd::syncShared, cmd::lockPart, cmd::unlockPart})
                m.addCommandItem(c, id);
            m.addSeparator();
            m.addCommandItem(c, cmd::importPicture);
            m.addCommandItem(c, cmd::importAudio);
            m.addCommandItem(c, cmd::importMusicXml);
            m.addSeparator();
            for (auto id : {cmd::save, cmd::saveAs, cmd::saveOnStop, cmd::saveAsTemplate, cmd::scoreInfo, cmd::printPreview, cmd::collectPackage,
                            cmd::relinkMedia})
                m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::exportStems, cmd::exportPicture, cmd::exportMidi, cmd::exportMusicXml, cmd::exportFullScorePdf,
                            cmd::exportPartsPdf, cmd::exportScoreSvg})
                m.addCommandItem(c, id);
            break;
        case 1:
            m.addCommandItem(c, cmd::undo);
            m.addCommandItem(c, cmd::redo);
            m.addSeparator();
            for (auto id : {cmd::editShuffle, cmd::editSlip, cmd::editSpot, cmd::editGrid, cmd::toggleSmartTool})
                m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::groupClips, cmd::ungroupClips, cmd::reverseClips, cmd::timeStretchClips}) m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::lyrics, cmd::addRepeat, cmd::unrollRepeats, cmd::explodeTracks, cmd::implodeTracks})
                m.addCommandItem(c, id);
            m.addCommandItem(c, cmd::spreadChords);
            m.addSubMenu("Send Notes from the Selected Track", sketch_menu(context_, context_.session.selected_track()),
                         context_.session.data().find_track(context_.session.selected_track()) != nullptr);
            m.addSeparator();
            m.addCommandItem(c, cmd::hearEntry);
            m.addSubMenu("Keyboard Shortcuts", shortcuts_menu());
            break;
        case 2:
            for (auto id : {cmd::showComposer, cmd::showSequencer, cmd::toggleView, cmd::showMixer, cmd::showVideo,
                            cmd::showCueManager})
                m.addCommandItem(c, id);
            m.addSeparator();
            m.addCommandItem(c, cmd::scoreView);
            m.addCommandItem(c, cmd::toggleWorkView);
            m.addCommandItem(c, cmd::toggleDock);
            m.addCommandItem(c, cmd::trackList);
            m.addSubMenu("Layouts", layouts_menu());
            m.addSeparator();
            m.addCommandItem(c, cmd::toggleFollow);
            m.addCommandItem(c, cmd::showRouting);
            m.addCommandItem(c, cmd::showReview);
            m.addCommandItem(c, cmd::showComments);
            m.addCommandItem(c, cmd::addComment);
            m.addCommandItem(c, cmd::showCircleOfFifths);
            m.addCommandItem(c, cmd::toggleStaffTools);
            m.addCommandItem(c, cmd::detachStaffTools);
            m.addSeparator();
            m.addCommandItem(c, cmd::zoomIn);
            m.addCommandItem(c, cmd::zoomOut);
            m.addSeparator();
            m.addSubMenu("Theme", theme_menu());
            m.addCommandItem(c, cmd::showControlGallery);
            break;
        case 3:
            for (auto id : {cmd::play, cmd::rewind}) m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::toggleLoop, cmd::toggleRecord, cmd::togglePunch}) m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::toggleMetronome, cmd::addHit, cmd::tempoFinder, cmd::monitorDim, cmd::captureMidi}) m.addCommandItem(c, id);
            break;
        case 4:
            for (auto id : {cmd::addTrack, cmd::addAudioTrack}) m.addCommandItem(c, id);
            for (auto id : {cmd::addAuxBus, cmd::addGroupBus}) m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::duplicateVersion, cmd::loadInstrument, cmd::openInstrumentWindow, cmd::articulationMap,
                            cmd::missingInstruments, cmd::bounceInPlace, cmd::freezeTrack, cmd::freezeUnload, cmd::unfreezeTrack,
                            cmd::rerenderTrack, cmd::expandArticulations, cmd::consolidateArticulations, cmd::removeTrack})
                m.addCommandItem(c, id);
            m.addCommandItem(c, cmd::mixTools);
            m.addSeparator();
            m.addItem("Check Instrument Ranges...", [this] { TrackPanel::show_range_report(context_, {}); });
            m.addSeparator();
            {
                const std::string track = context_.session.selected_track();
                const bool shown = !track.empty() && sequencer_.automation_shown(track);
                m.addItem(shown ? "Hide Automation" : "Show Automation", !track.empty(), false, [this, track, shown] {
                    sequencer_.show_automation(track, !shown);
                    if (!shown) show_workspace(Workspace::Sequencer);
                });
                m.addItem("Show Master Automation", true, sequencer_.automation_shown("bus-master"), [this] {
                    const bool on = !sequencer_.automation_shown("bus-master");
                    sequencer_.show_automation("bus-master", on);
                    if (on) show_workspace(Workspace::Sequencer);
                });
                m.addItem("Hide All Automation", [this] { sequencer_.hide_all_automation(); });
            }
            break;
        case 5:
            for (auto id : {cmd::chordExplorer, cmd::ostinatoCreator}) m.addCommandItem(c, id);
            m.addSeparator();
            m.addCommandItem(c, cmd::instrumentUsage);
            break;
        case 6:
            m.addCommandItem(c, cmd::account);
            m.addCommandItem(c, cmd::checkUpdates);
            m.addSeparator();
            m.addCommandItem(c, cmd::credits);
            m.addCommandItem(c, cmd::about);
            break;
        case 7:  // Templates: whole scoring palettes (horror, comedy, action...) as ready-made tracks
            m.addCommandItem(c, cmd::addTemplate);
            m.addCommandItem(c, cmd::newFromTemplate);
            m.addCommandItem(c, cmd::newProjectWizard);
            m.addSeparator();
            m.addCommandItem(c, cmd::saveAsTemplate);
            m.addItem("Import or Export Templates...", [this] { show_template_browser(context_, false); });
            break;
        case 8: {  // Window: every window, and the ones open now
            for (auto id : {cmd::showComposer, cmd::showSequencer}) m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::showMixer, cmd::showVideo, cmd::showCueManager, cmd::showRouting, cmd::showReview,
                            cmd::showComments, cmd::instrumentUsage})
                m.addCommandItem(c, id);
            m.addItem("Mixer Window", [this] { show_console_window(); });
            m.addSeparator();
            for (auto id : {cmd::addTemplate, cmd::chordExplorer, cmd::ostinatoCreator, cmd::showCircleOfFifths})
                m.addCommandItem(c, id);
            m.addSeparator();
            m.addCommandItem(c, cmd::toggleInspector);
            m.addCommandItem(c, cmd::libraryWindow);
            m.addCommandItem(c, cmd::soundAssignments);
            m.addCommandItem(c, cmd::trackList);
            m.addSeparator();
            m.addCommandItem(c, cmd::account);
            const auto open = dialogs::open_tool_windows();
            if (!open.empty()) {
                m.addSeparator();
                m.addSectionHeader("Open windows");
                for (auto* w : open) {
                    juce::Component::SafePointer<dialogs::ToolWindow> safe(w);
                    m.addItem(w->getName(), [safe] {
                        if (safe == nullptr) return;
                        safe->setMinimised(false);
                        safe->toFront(true);
                    });
                }
            }
            break;
        }
        case 9:  // Studio: the hardware - interface, MIDI, plug-ins, surfaces, displays
            m.addCommandItem(c, cmd::audioSettings);
            m.addCommandItem(c, cmd::midiSetup);
            m.addSeparator();
            m.addCommandItem(c, cmd::scanPlugins);
            m.addCommandItem(c, cmd::reloadCrashedPlugins);
            m.addCommandItem(c, cmd::builtinSounds);
            m.addSeparator();
            m.addCommandItem(c, cmd::mixerController);
            m.addCommandItem(c, cmd::showControlSurfaces);
            m.addCommandItem(c, cmd::midiInputFilter);
            m.addSeparator();
            m.addSubMenu("Mixer Window on Display", mixer_display_menu());
            break;
        default: break;
    }
    return m;
}


void MainComponent::offer_new_part(const std::string& part_id)
{
    const auto* part = context_.session.data().find_track(part_id);
    if (!part || offering_part_) return;
    const juce::String name(part->name);
    auto make = [this, part_id, name] {
        const auto id = context_.session.add_part_from(part_id);
        if (!id.empty())
            context_.status("Made " + juce::String(context_.session.data().find_track(id)->name) +
                            ": write the overlapping music there - it prints as its own part");
    };
    if (answer_new_part) {
        if (answer_new_part()) make();
        return;
    }
    offering_part_ = true;
    juce::Component::SafePointer<MainComponent> self(this);
    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::QuestionIcon, "One articulation per bar",
        juce::String(context_.session.last_refusal()) + ".\n\nMake a new part of " + name + " (" + name +
            " div.) for the overlapping music? It gets its own staff and prints as its own part.",
        "New Part", "Cancel", nullptr, juce::ModalCallbackFunction::create([self, make](int r) {
            if (!self) return;
            self->offering_part_ = false;
            if (r == 1) make();
        }));
}

}  // namespace vcc::desktop
