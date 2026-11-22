// Instrument Sounds: the full-screen page where every instrument gets its
// sound from your own libraries, articulation by articulation. Pick an
// instrument and an articulation (Violins I - Legato), choose the player
// (EastWest Opus or PLAY, Kontakt...) from a menu as a DAW's instrument
// insert does, and its own window opens, floating, to load the patch; play
// your keyboard to hear it; Save. That sound is then the default: every
// template, new track and articulation track for Violins I legato gets it.
// You can add instruments of your own and articulations to any instrument.
#pragma once

#include <string>
#include <vector>

#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"

namespace vcc::desktop {

class InstrumentSoundsPage final : public juce::Component, private juce::ChangeListener {
public:
    explicit InstrumentSoundsPage(AppContext& context);
    ~InstrumentSoundsPage() override;

    /// An instrument (registry or your own) and its articulation rows.
    void show_instrument(const std::string& instrument_id);
    const std::string& instrument() const noexcept { return instrument_; }
    /// The articulation rows: "" (the whole instrument), then articulations.
    const std::vector<std::string>& articulations() const noexcept { return articulations_; }
    void select_articulation(const std::string& articulation);
    const std::string& articulation() const noexcept { return articulation_; }
    /// The instruments listed (ids), after the search.
    std::vector<std::string> listed_instruments() const;
    void set_search(const juce::String& text);

    /// Loads a player (plug-in identifier) into the Sound Setup slot and opens
    /// its window; your keyboard plays it. Returns what happened.
    juce::String load_player(const std::string& identifier);
    /// Opens the loaded player's window again.
    juce::String open_player_window();
    /// Saves the player as now set up as the sound for the instrument and
    /// articulation chosen (the default from now on), named `name`. With
    /// `fill_empty`, also for every articulation of it without a sound yet.
    juce::String save_sound(const juce::String& name, bool fill_empty = false);
    /// Loads the assigned sound (with its settings) into the slot to change it.
    juce::String edit_assigned();
    /// Clears the chosen articulation's sound.
    void clear_assigned();
    /// Gives the chosen articulation a sound you saved before (patch id). Returns what happened.
    juce::String use_saved(const std::string& patch_id);
    /// The sound now assigned to an articulation row, or empty.
    juce::String assigned_name(const std::string& articulation) const;
    /// Adds an instrument of your own, based on a registry instrument; shows it.
    std::string add_instrument(const juce::String& name, const std::string& based_on);
    void add_articulation(const std::string& articulation);
    /// The name offered for saving ("Violins I - Legato").
    juce::String suggested_name() const;
    /// Closes the setup slot (the page going away).
    void release_slot();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class List;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void rebuild_instruments();
    void rebuild_articulations();
    void refresh_detail();
    void show_player_menu();
    void show_add_instrument();
    void show_add_articulation();
    juce::String instrument_name(const std::string& id) const;
    std::string family_of(const std::string& id) const;

    AppContext& context_;
    std::string instrument_, articulation_;
    std::vector<std::string> articulations_;
    struct Row {
        bool header = false;
        std::string id;
        juce::String text;
    };
    std::vector<Row> rows_;
    juce::String search_text_;
    std::string loaded_player_;  ///< Plug-in identifier in the setup slot.
    std::unique_ptr<List> instruments_, artics_;
    juce::TextEditor search_;
    juce::Label title_, subtitle_, current_, hint_, status_, name_label_, step1_, step2_, step3_, instruments_title_, artics_title_;
    juce::TextEditor name_;
    hw::Keycap choose_{"Choose Player"}, open_{"Open Player Window"}, save_{"Save Sound"}, edit_{"Edit Saved Sound"},
        clear_{"Clear"}, add_instrument_{"+ Instrument"}, add_articulation_{"+ Articulation"};
    juce::ToggleButton fill_empty_{"Also use it for this instrument's articulations that have no sound yet"};
    juce::Label saved_label_;
    juce::ComboBox saved_;  ///< Your saved sounds: this instrument's first.
    std::vector<std::string> saved_ids_;
    void refresh_saved();
};

}  // namespace vcc::desktop
