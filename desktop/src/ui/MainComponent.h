// The main window's content (Main Spec §4): transport bar, the persistent
// track panel, the Composer and Sequencer workspaces, the status bar, menus
// and every application command.
#pragma once

#include <map>

#include "ui/AppContext.h"
#include "ui/ComposerView.h"
#include "ui/MixerComponent.h"
#include "ui/SequencerView.h"
#include "ui/TrackPanel.h"
#include "ui/composer/Inspector.h"
#include "ui/shell/KeyboardDeck.h"
#include "ui/shell/MasterPanel.h"
#include "ui/shell/MixerDeck.h"
#include "ui/shell/PrintPanel.h"
#include "ui/shell/ShellPanels.h"
#include "ui/shell/TopDeck.h"

namespace vcc::desktop {

class MainComponent final : public juce::Component,
                            public juce::ApplicationCommandTarget,
                            public juce::MenuBarModel,
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
    void open_file(const juce::File& file);
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
    /// An insert's window: a hosted effect's editor, or a built-in processor's controls.
    void show_insert_window(const std::string& owner_id, int slot);

    /// Places audio files at the playhead on the selected audio track (or a
    /// new one) as one undo step, copying them into the project's "Audio
    /// Files" folder when the project is saved. Returns the clip ids.
    std::vector<std::string> import_audio_files(const juce::Array<juce::File>& files);
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
    shell::SelectionPanel& selection_panel() noexcept { return selection_; }
    composer_ui::Inspector& inspector() noexcept { return inspector_; }
    shell::KeyboardDeck& keyboard() noexcept { return keyboard_; }
    shell::MixerDeck& mixer_deck() noexcept { return mixer_deck_; }
    /// The Producer view's bottom deck: the mixer (default) or the keyboard.
    void show_bottom_mixer(bool mixer);
    bool bottom_mixer() const noexcept { return bottom_mixer_; }
    shell::ClipsPanel& clips_panel() noexcept { return clips_; }
    shell::PrintPanel& print_panel() noexcept { return print_; }
    shell::MasterPanel& master_panel() noexcept { return master_; }
    shell::ChannelPanel& channel_panel() noexcept { return channel_; }
    shell::StatusStrip& status_strip() noexcept { return status_strip_; }
    /// The stage bar: 0 Compose, 1 Score, 2 Mix, 3 Master.
    void go_to_stage(int stage);

    ComposerView& composer() noexcept { return composer_; }
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
    class BuiltinInsertWindow;
    std::string insert_kind(const std::string& owner_id, int slot) const;
    void timerCallback() override;
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
    shell::EditFrame edit_;
    shell::MarkersPanel markers_;
    shell::NewTrackPanel new_track_;
    shell::ClipsPanel clips_;
    shell::SelectionPanel selection_;
    composer_ui::Inspector inspector_;
    shell::ChannelPanel channel_;
    shell::PrintPanel print_;
    shell::MasterPanel master_;
    bool master_stage_ = false;  ///< The Master stage: the right column is the Master panel.
    shell::StatusStrip status_strip_;
    shell::KeyboardDeck keyboard_;
    shell::MixerDeck mixer_deck_;
    bool bottom_mixer_ = true;
    TrackPanel tracks_;
    juce::StretchableLayoutManager layout_;
    std::unique_ptr<juce::StretchableLayoutResizerBar> divider_;
    ComposerView composer_;
    SequencerView sequencer_;
    Workspace workspace_ = Workspace::Composer;
    std::unique_ptr<MixerWindow> mixer_;
    std::map<std::string, std::unique_ptr<PluginWindow>> plugin_windows_;
    std::map<std::string, std::unique_ptr<BuiltinInsertWindow>> builtin_windows_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::unique_ptr<juce::DocumentWindow> gallery_window_;
    int ticks_ = 0;
    bool syncing_range_ = false;
    int status_age_ = 0;
    bool punch_ = false;
};

}  // namespace vcc::desktop
