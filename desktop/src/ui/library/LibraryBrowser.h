// The Library browser (Main Spec §10), the Library tab of the left panel:
// search and filter every sound the composer can use - saved patches, the
// catalog's libraries for the players installed here, and the instrument
// plug-ins the scan found - see candidates ranked for the selected track
// (exact, fallback or inferred), audition them through their own plug-in on a
// temporary preview channel, assign one to the track (one undo step) and save
// the track's sound as a patch, so templates build with it next time.
#pragma once

#include <memory>

#include "ui/AppContext.h"
#include "vcc/library/Library.h"

namespace vcc::desktop {

/// Owns the composer's library: loads the catalog and the user's file
/// (Library.json in the app's data folder), follows the plug-in scan, and
/// gives the session the saved patches for template building.
class LibraryService final : public juce::ChangeBroadcaster, private juce::ChangeListener {
public:
    explicit LibraryService(AppContext& context);
    ~LibraryService() override;

    library::Library& lib() noexcept { return lib_; }
    /// Re-reads the scanned instrument plug-ins and the library folders.
    void refresh();
    /// Saves the user's library and updates the session (after any change).
    void changed();
    juce::File file() const;
    /// Points saving somewhere else (checks); loads what is there.
    void set_file(const juce::File& f);

    /// The plug-in that loads an entry, as a reference (no state).
    project::PluginRef plugin_ref(const std::string& identifier) const;
    /// Plays a short phrase through the entry's plug-in (its saved state for
    /// a patch); with no plug-in, the placeholder voice of its instrument.
    juce::String audition(const library::Entry& e);
    /// Puts the entry's sound on a track, saying how the track's request was
    /// met. Returns what happened.
    juce::String assign(const library::Entry& e, const std::string& track_id, bool open_window = true);
    /// Gives every instrument track without a plug-in of its own the best
    /// sound here for its instrument and articulations: saved sounds first,
    /// then the catalog's patches a player here can load (EastWest Opus or
    /// PLAY, Kontakt, SINE...), preferring `prefer_library` (a catalog id)
    /// when given. Returns what happened, for the status line.
    juce::String assign_from_libraries(const std::string& prefer_library);
    /// The libraries this computer has (a player installed and the library
    /// found), as (catalog id, name).
    std::vector<std::pair<std::string, std::string>> installed_libraries() const;
    /// A saved sound for an entry, made if it is not one (a catalog patch or a
    /// plug-in: its plug-in, for this instrument and articulation). Returns
    /// its id, or empty when nothing here can play it.
    std::string saved_sound_for(const library::Entry& e, const std::string& instrument_id, const std::string& articulation);
    /// Gives the session the sound assignments (after any change).
    void push_assignments();
    /// Saves a track's current sound (plug-in and state) as a verified patch.
    /// Returns the patch id, or empty with `message` set.
    std::string save_track_sound(const std::string& track_id, const std::string& display, juce::String& message);

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }
    AppContext& context_;
    library::Library lib_;
    juce::File file_;
};

/// The Library tab.
class LibraryBrowser final : public juce::Component, private juce::ListBoxModel, private juce::ChangeListener {
public:
    explicit LibraryBrowser(AppContext& context);
    ~LibraryBrowser() override;

    enum class View { Sounds, ForTrack, Instruments };
    void set_view(View v);
    View view() const noexcept { return view_; }
    juce::TextEditor& search() noexcept { return search_; }
    /// What is listed: entry keys (Sounds, ForTrack) or instrument ids.
    std::vector<std::string> listed() const;
    void select(int row);
    int selected() const { return list_.getSelectedRow(); }
    /// The detail line under the list.
    juce::String detail() const { return detail_.getText(); }
    void refilter();

    // The keys, for checks.
    void audition_selected();
    /// Assign: in Sounds, opens Sound Assignments with the sound; else Put on Track.
    void assign_selected();
    /// Puts the selected sound on the selected track (one undo step).
    void put_on_track_selected();
    void toggle_favorite_selected();
    void save_track_sound();

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
    void selectedRowsChanged(int row) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void show_more_menu();
    const library::Entry* entry_at(int row) const;

    AppContext& context_;
    View view_ = View::Sounds;
    juce::TextEditor search_;
    juce::ComboBox view_box_, family_box_, technique_box_;
    juce::ToggleButton installed_{"Installed"}, favorites_{"Favorites"};
    juce::ListBox list_;
    juce::Label detail_;
    juce::TextButton audition_{"Audition"}, assign_{"Assign"}, star_{"Favorite"}, save_{"Save sound"},
        more_{"More..."};
    std::vector<library::Entry> entries_;
    std::vector<library::Candidate> candidates_;
    std::vector<const templates::InstrumentDef*> instruments_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::string track_seen_;
    int listener_ = -1;
};

}  // namespace vcc::desktop
