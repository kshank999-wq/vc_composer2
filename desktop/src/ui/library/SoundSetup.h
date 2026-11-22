// Sound Setup: the big window for putting your own libraries on the tracks,
// as in any DAW. Pick a track, pick the player (EastWest Opus or PLAY,
// Kontakt, SINE...) and its own window opens to load a patch in it; then
// name the sound ("Violins I - Pizzicato"), say which articulation it plays,
// and save it. A saved sound is kept with the plug-in's settings, used for
// that instrument and articulation in every template and new track, and
// can be put on another track with one click.
#pragma once

#include <string>
#include <vector>

#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"
#include "ui/library/InstrumentSounds.h"

namespace vcc::desktop {

class SoundSetupPanel final : public juce::Component, private juce::ChangeListener {
public:
    explicit SoundSetupPanel(AppContext& context);
    ~SoundSetupPanel() override;

    /// Shows a track (and selects it in the project).
    void show_track(const std::string& track_id);
    const std::string& track() const noexcept { return track_; }
    /// The tracks listed: every instrument track, in project order.
    const std::vector<std::string>& tracks() const noexcept { return tracks_; }
    /// The players listed: (display, plug-in identifier).
    const std::vector<std::pair<juce::String, std::string>>& players() const noexcept { return players_; }
    /// Puts a player (by plug-in identifier) on the track and opens its window. Returns what happened.
    juce::String load_player(const std::string& identifier);
    /// Saves the track's sound (the player as now set up) under `name` for
    /// `articulation` ("" = the whole instrument), used for that instrument
    /// and articulation from now on when `use_everywhere`. Returns what happened.
    juce::String save_sound(const juce::String& name, const std::string& articulation, bool use_everywhere);
    /// Puts a saved sound (patch id) on the track. Returns what happened.
    juce::String use_saved(const std::string& patch_id);
    /// The saved sounds for the track's instrument: (name, patch id).
    std::vector<std::pair<juce::String, std::string>> saved_for_track() const;
    /// The name offered for saving: "<track> - <articulation>".
    juce::String suggested_name() const;
    juce::TextEditor& name_editor() noexcept { return name_; }
    /// The Libraries page (where each library is, scan, locate) or the Sounds page.
    void show_libraries(bool on) { show_page(on ? 2 : 1); }
    bool libraries_shown() const noexcept { return page_ == 2; }
    /// 0 Instruments (every instrument's sounds), 1 Tracks, 2 Libraries.
    void show_page(int page);
    int page() const noexcept { return page_; }
    InstrumentSoundsPage& instruments_page() noexcept { return *instruments_page_; }
    /// Points a missing library at a folder you chose. Returns what happened.
    juce::String locate(std::size_t index, const juce::File& folder);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class TrackList;
    class PlayerList;
    class SavedList;
    void refresh_tracks();
    void refresh_track();
    void refresh_players();
    juce::String open_player_window();
    void next_track(int step = 1);
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void refresh_libraries();
    void layout_libraries(juce::Rectangle<int> area);

    AppContext& context_;
    int listener_ = -1;
    int left_w_ = 320;
    static constexpr int kTopBar = 64;
    std::string track_;
    std::vector<std::string> tracks_;
    std::vector<std::pair<juce::String, std::string>> players_;
    std::vector<std::string> articulations_;  ///< In the combo, after "Whole instrument".
    std::unique_ptr<TrackList> track_list_;
    std::unique_ptr<PlayerList> player_list_;
    std::unique_ptr<SavedList> saved_list_;
    juce::Label tracks_title_, track_title_, track_info_, step1_, step2_, step3_, step2_text_, saved_title_, status_;
    hw::Keycap load_{"Load Player"}, open_window_{"Open Player Window"}, save_{"Save Sound"}, use_saved_{"Put on Track"},
        next_{"Next Track"}, prev_{"Previous Track"}, scan_{"Scan Plug-ins"}, full_{"Full Screen"};
    juce::Label name_label_, artic_label_;
    juce::TextEditor name_;
    juce::ComboBox articulation_;
    juce::ToggleButton everywhere_{"Use it for this instrument and articulation in every project"};
    // The Libraries page.
    int page_ = 0;
    hw::Keycap instruments_tab_{"Instruments", true}, sounds_tab_{"Tracks", true}, libraries_tab_{"Libraries", true};
    std::unique_ptr<InstrumentSoundsPage> instruments_page_;
    class LibraryList;
    std::unique_ptr<LibraryList> library_list_;
    juce::Label libraries_title_, libraries_hint_;
    hw::Keycap scan_drives_{"Scan All Drives"}, add_folder_{"Add Folder..."}, locate_{"Locate..."}, remove_{"Remove"},
        recheck_{"Check Again"};
};

void show_sound_setup(AppContext& context);
SoundSetupPanel* sound_setup_panel();

}  // namespace vcc::desktop
