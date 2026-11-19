// Sound Assignments: the composer's chosen sound for each instrument and
// articulation ("Violins I - Legato" -> the saved sound they like), chosen
// once and used wherever that instrument and articulation come up: a new
// track, a template, an articulation track. Kept with the user's library,
// exported for safekeeping or another computer, imported back.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ui/AppContext.h"
#include "vcc/library/Library.h"

namespace vcc::desktop {

class SoundAssignmentsPanel final : public juce::Component, private juce::ListBoxModel {
public:
    explicit SoundAssignmentsPanel(AppContext& context);
    ~SoundAssignmentsPanel() override;

    /// The sound the Library's Assign carried in (assigned with each row's Use key).
    void set_carried(std::optional<library::Entry> entry);
    /// Shows an instrument's articulations.
    void show_instrument(const std::string& instrument_id);
    const std::string& instrument() const noexcept { return instrument_; }
    /// The rows shown: "" (the whole instrument), then articulations.
    const std::vector<std::string>& rows() const noexcept { return rows_; }
    /// Assigns the carried sound to a row. Returns what happened.
    juce::String use_carried(const std::string& articulation);
    /// Clears a row.
    void clear(const std::string& articulation);
    /// The assigned sound's name for a row, or empty.
    juce::String assigned_text(const std::string& articulation) const;
    /// Puts the assignments on this project's matching tracks now (replacing their sounds). Returns how many.
    int apply_to_project();
    /// Writes / reads a file of the assigned sounds (with their settings).
    juce::String export_to(const juce::File& file);
    juce::String import_from(const juce::File& file);
    /// The articulations an instrument can be given a sound for: those the
    /// templates use for it, those its catalog patches cover, those assigned.
    std::vector<std::string> articulations_for(const std::string& instrument_id) const;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class Row;
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void selectedRowsChanged(int row) override;
    void refilter();
    void rebuild_rows();
    void choose_menu(const std::string& articulation, juce::Component* target);
    void assign(const std::string& articulation, const std::string& patch_id);

    AppContext& context_;
    std::optional<library::Entry> carried_;
    juce::Label carried_label_, title_, hint_;
    juce::ComboBox family_;
    juce::TextEditor search_;
    juce::ListBox instruments_list_{"Instruments", this};
    std::vector<const templates::InstrumentDef*> instruments_;
    std::string instrument_;
    std::vector<std::string> rows_;
    juce::Viewport rows_view_;
    juce::Component rows_holder_;
    std::vector<std::unique_ptr<Row>> row_comps_;
    juce::TextButton add_articulation_{"Add Articulation..."}, apply_{"Apply to This Project"}, export_{"Export..."},
        import_{"Import..."};
    std::unique_ptr<juce::FileChooser> chooser_;
};

/// Opens (or brings forward) the Sound Assignments window, with `carried`
/// (the Library's selection) ready to assign; on its instrument when it has one.
void show_sound_assignments(AppContext& context, std::optional<library::Entry> carried = std::nullopt);
SoundAssignmentsPanel* sound_assignments_panel();

}  // namespace vcc::desktop
