// The main window's content (Main Spec §4): transport bar, the persistent
// track panel, the Composer and Sequencer workspaces, the status bar, menus
// and every application command.
#pragma once

#include "ui/shell/Layouts.h"
#include "ui/surface/MixerController.h"

#include <map>
#include <thread>
#include <atomic>
#include <set>

#include "ui/AppContext.h"
#include "ui/ComposerView.h"
#include "ui/composer/PageView.h"
#include "ui/Import.h"
#include "ui/MixerComponent.h"
#include "ui/SequencerView.h"
#include "ui/TrackPanel.h"
#include "ui/composer/Inspector.h"
#include "ui/Dialogs.h"
#include "ui/shell/KeyboardDeck.h"
#include "ui/shell/MasterPanel.h"
#include "ui/shell/MixerDeck.h"
#include "ui/shell/PrintPanel.h"
#include "ui/shell/ShellPanels.h"
#include "ui/shell/TopDeck.h"
#include "ui/surface/ControlSurface.h"

namespace vcc::desktop {

class MainComponent final : public juce::Component,
                            public juce::ApplicationCommandTarget,
                            public juce::MenuBarModel,
                            public juce::FileDragAndDropTarget,
                            private juce::Timer {
public:
    explicit MainComponent(AppContext& context);
    ~MainComponent() override;

    /// The two views of the one project (docs/spec/ui): Composer (the score)
    /// and Producer (MIDI, audio, mixing). `Sequencer` is the Producer view's
    /// old name.
    enum class Workspace { Composer, Producer, Sequencer = Producer };
    void show_workspace(Workspace w);
    Workspace workspace() const noexcept { return workspace_; }

    /// True when it is fine to discard the document (saved, or the user agreed).
    bool confirm_discard();
    /// A newer build vc-composer.com offers (empty: none seen).
    juce::String update_waiting_;
    /// File > Shared Projects opens a downloaded project through this (asks about unsaved work first).
    std::function<bool(const juce::File&)> open_shared_file();
    void open_file(const juce::File& file);
    /// Unpacks a package .zip beside it and opens the project inside.
    void open_package(const juce::File& zip);
    bool save(bool save_as);
    void show_mixer();
    /// Every hardware control of the UI spec, in the current theme.
    void show_control_gallery();
    juce::PopupMenu theme_menu();
    /// The shortcut profiles: Composer 2 (the default), then the built-in ones.
    juce::PopupMenu shortcuts_menu();
    /// Switches the keyboard profile by name, remembering it. False when unknown.
    bool use_shortcut_profile(const std::string& name);
    juce::DocumentWindow* control_gallery() noexcept { return gallery_window_.get(); }
    void import_musicxml();
    void export_musicxml();
    void export_parts_pdf_dialog();
    void export_full_score_dialog();
    void show_plugin_window(const std::string& track_id);
    void show_plugin_window_when_loaded(const std::string& track_id, int attempt);
    /// An insert's window: a hosted effect's editor, or a built-in processor's controls.
    void show_insert_window(const std::string& owner_id, int slot);

    /// Places audio files at the playhead on the selected audio track (or a
    /// new one) as one undo step, copying them into the project's "Audio
    /// Files" folder when the project is saved. Returns the clip ids.
    std::vector<std::string> import_audio_files(const juce::Array<juce::File>& files);
    /// Imports audio and MIDI files (drag and drop, menus): onto `track_id`
    /// when it suits, else the selected or new tracks, from `at_seconds`.
    ImportResult import_files(const juce::Array<juce::File>& files, const std::string& track_id, double at_seconds);
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    /// Punch in/out on the sequencer's ruler range (off when there is none).
    void set_punch(bool on);
    /// Loop playback over the shared selection's range (one bar at the
    /// playhead when there is none).
    void set_loop(bool on);
    bool punch() const noexcept { return punch_; }
    shell::TopDeck& deck() noexcept { return deck_; }
    shell::Toolbar& toolbar() noexcept { return toolbar_; }
    shell::EditFrame& edit_frame() noexcept { return edit_; }
    shell::MarkersPanel& markers() noexcept { return markers_; }
    shell::NewTrackPanel& new_track_panel() noexcept { return new_track_; }
    TrackPanel& track_panel() noexcept { return tracks_; }
    shell::SelectionPanel& selection_panel() noexcept { return selection_; }
    composer_ui::Inspector& inspector() noexcept { return inspector_; }
    /// Window > Inspector: what is selected and the inspector's keys and
    /// knobs, in a floating window (movable to another screen). Off unless
    /// turned on (the staff tools are at hand anyway); remembered.
    void set_inspector_shown(bool shown);
    /// Window > Library: the sound library in a floating window (another screen too).
    void set_library_shown(bool shown);
    bool library_shown() const;
    bool inspector_shown() const;
    dialogs::ToolWindow* inspector_window() const { return inspector_window_.getComponent(); }
    shell::KeyboardDeck& keyboard() noexcept { return keyboard_; }
    shell::MixerDeck& mixer_deck() noexcept { return mixer_deck_; }
    /// The Producer view's bottom deck: the mixer (default) or the keyboard.
    void show_bottom_mixer(bool mixer);
    /// The mixer deck in a window of its own (drag it to another screen; it
    /// opens where it was left), or back in the bottom deck. Closing the
    /// window docks it.
    void set_mixer_popped(bool popped);
    bool mixer_popped() const noexcept { return deck_window_ != nullptr; }
    juce::DocumentWindow* mixer_deck_window() noexcept { return deck_window_.get(); }
    /// The mixer full screen (the whole window under the toolbar, faders as
    /// long as it allows) or compact (the bottom deck). Cmd/Ctrl+M and the
    /// toolbar's Mixer key toggle it; the deck's own key switches size.
    void set_mixer_full(bool full, bool compact_after = true);
    /// The full mixer: its own window, open or not.
    bool mixer_full() const noexcept { return mixer_ != nullptr && mixer_->isVisible(); }
    /// The console window: every strip's EQ, dynamics, sends and inserts.
    /// On `display` (a key from display_keys(), e.g. a touch screen), filling
    /// it; empty: where it was. Remembered, and with "Open Mixer at Start-up"
    /// it opens there each time.
    void show_console_window(const juce::String& display = {});
    /// The displays now connected: (key, "Display 2 - 1920 x 1080").
    static std::vector<std::pair<juce::String, juce::String>> display_keys();
    juce::PopupMenu mixer_display_menu();
    const juce::String& console_display() const noexcept { return console_display_; }
    /// The full-screen mixer (the console, laid out like hardware), or null before it is first shown.
    MixerComponent* full_console() noexcept { return mixer_ != nullptr ? &mixer_->mixer() : nullptr; }
    /// The jog/shuttle and transport controller (Tools > Control Surfaces).
    surface_ui::ControlSurface& surface() noexcept { return *surface_; }
    /// The hardware mixer controller (Mackie Control), Studio > Mixer Controller.
    MixerController& mixer_controller() noexcept { return *mixer_controller_; }
    /// In a short window the Producer column shows Channel (0) or Print (1).
    void show_right_tab(int tab);
    bool right_tabs() const noexcept { return right_tabs_; }
    /// Follow on or off (the title bar's key, View > Follow Playhead, Cmd/Ctrl+Shift+F); remembered.
    void set_follow(bool on);
    bool bottom_mixer() const noexcept { return bottom_mixer_; }
    /// The bottom deck (keyboard or mixer) hidden, leaving a thin bar to bring it back.
    void set_dock_hidden(bool hidden);
    bool dock_hidden() const noexcept { return dock_hidden_; }
    /// Work view: the side panels hidden, the edit window as large as it gets.
    void set_work_view(bool on);
    /// View > Track List: the floating list of tracks; picking one goes to it.
    void toggle_track_list();
    bool track_list_shown() const;
    void go_to_track(const std::string& track_id);
    bool work_view() const noexcept { return work_view_; }
    /// View > Layouts: the arrangement now, and one applied in a step.
    layouts::ViewLayout capture_layout() const;
    void apply_layout(const layouts::ViewLayout& layout);
    shell::ClipsPanel& clips_panel() noexcept { return clips_; }
    shell::PrintPanel& print_panel() noexcept { return print_; }
    shell::MasterPanel& master_panel() noexcept { return master_; }
    shell::ChannelPanel& channel_panel() noexcept { return channel_; }
    shell::StatusStrip& status_strip() noexcept { return status_strip_; }
    /// The stage bar: 0 Compose, 1 Score, 2 Mix, 3 Master.
    void go_to_stage(int stage);

    ComposerView& composer() noexcept { return composer_; }
    /// The Composer view's Page view (the score on pages) or Track view.
    void set_page_view(bool page);
    bool page_view_shown() const noexcept { return page_mode_; }
    PageView& page_view() noexcept { return page_view_; }
    SequencerView& sequencer() noexcept { return sequencer_; }
    MixerWindow* mixer_window() noexcept { return mixer_.get(); }

    void paint(juce::Graphics& g) override;
    void resized() override;

    // ApplicationCommandTarget
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID>& commands) override;
    void getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) override;
    bool perform(const InvocationInfo& info) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
    void menuItemSelected(int, int) override {}

private:
    class PluginWindow;
    std::string insert_kind(const std::string& owner_id, int slot) const;
    void timerCallback() override;
    /// Lights the toolbar keys that mirror a mode (punch, pencil, edit mode,
    /// Smart tool, Chord mode) at once, not on the next timer tick.
    void refresh_tool_keys();
    void capture_plugin_states();
    void export_midi();
    void save_as_template();
    void show_audio_settings();
    void show_plugin_scanner();
    void show_about();
    void import_audio();
    void new_audio_track();
    void new_bus(bool fx);
    void toggle_record();

    AppContext& context_;
    int listener_ = 0;
    shell::TopDeck deck_;
    shell::Toolbar toolbar_;
    /// Notes outside each track's range, last counted (for the warning).
    std::map<std::string, std::set<notation::EventId>> range_counts_;
    bool range_counted_ = false;
    shell::EditFrame edit_;
    shell::MarkersPanel markers_;
    shell::NewTrackPanel new_track_;
    shell::ClipsPanel clips_;
    shell::SelectionPanel selection_;
    composer_ui::Inspector inspector_;
    std::unique_ptr<juce::Component> inspector_box_;  ///< Holds the selection readout and the inspector.
    juce::Component::SafePointer<dialogs::ToolWindow> inspector_window_;
    juce::Component::SafePointer<dialogs::ToolWindow> library_window_;
    shell::ChannelPanel channel_;
    shell::PrintPanel print_;
    shell::MasterPanel master_;
    bool master_stage_ = false;
    hw::Keycap channel_tab_{"Channel"}, print_tab_{"Print"};
    int right_tab_ = 0;
    bool right_tabs_ = false;  ///< The Master stage: the right column is the Master panel.
    shell::StatusStrip status_strip_;
    shell::KeyboardDeck keyboard_;
    shell::MixerDeck mixer_deck_;
    PageView page_view_;
    bool page_mode_ = false;
    bool bottom_mixer_ = true;
    bool dock_hidden_ = false;
    bool work_view_ = false;
    juce::String console_display_;
    hw::Keycap dock_key_{"Hide"};
    /// Which side panels show for the view (none in the Work view).
    void apply_side_visibility();
    juce::PopupMenu layouts_menu();
    juce::PopupMenu recent_menu();
    /// Where Open and Save As start: the project's folder, else the last one used.
    juce::File project_folder_for_dialogs() const;
    void save_layout_as();
    TrackPanel tracks_;
    bool was_playing_ = false;
    juce::StretchableLayoutManager layout_;
    std::unique_ptr<juce::StretchableLayoutResizerBar> divider_;
    ComposerView composer_;
    SequencerView sequencer_;
    std::unique_ptr<surface_ui::ControlSurface> surface_;
    std::unique_ptr<MixerController> mixer_controller_;
    std::uint64_t refusals_seen_ = 0;
    /// One articulation per bar: asks whether to make a new part ("div.") of
    /// `part_id` for the music that overlapped, and makes it on OK.
    void offer_new_part(const std::string& part_id);
    bool offering_part_ = false;
public:
    /// For the checks: what the last offer would make, answered at once.
    std::function<bool()> answer_new_part;
private:
    /// The autosave being written (serializing a large project off the message thread).
    std::thread autosave_thread_;
    std::atomic<bool> autosave_running_{false};
    Workspace workspace_ = Workspace::Composer;
    std::unique_ptr<MixerWindow> mixer_;
    /// The mixer deck popped out (it holds mixer_deck_, not owning it).
    std::unique_ptr<juce::DocumentWindow> deck_window_;
    /// The popped-out mixer's height: every strip's inserts, pan and a long fader.
    static constexpr int kPoppedMixerHeight = 680, kPoppedMixerMinHeight = 520;
    std::map<std::string, std::unique_ptr<PluginWindow>> plugin_windows_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::unique_ptr<juce::DocumentWindow> gallery_window_;
    int ticks_ = 0;
    bool syncing_range_ = false;
    int status_age_ = 0;
    std::set<std::string> unload_after_print_;
    std::map<std::string, project::FreezeState> freeze_cache_;
    std::uint64_t freeze_cache_generation_ = ~0ull;  ///< Freeze & Unload: close these once printed.
    bool punch_ = false;
};

}  // namespace vcc::desktop
