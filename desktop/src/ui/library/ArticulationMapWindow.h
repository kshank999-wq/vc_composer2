// Track > Articulation Map... (Main Spec §8, §11): how a track's written
// articulations and dynamics reach its instrument, edited in plain rows -
// one per articulation, plus the natural switch for plain notes: a
// keyswitch, a controller, a program change, a MIDI channel, a velocity
// layer, or a velocity/length shape. Learn takes the next key, controller or
// program sent from a MIDI keyboard. The health line says what is written
// but not heard. Apply is one undo step; the map is saved with the project,
// and with the track's saved library patch when it has one.
//
// Track > Missing Instruments: after opening a project whose plug-ins are not
// installed here, lists them with a Find substitute key each (the library
// browser's candidates for that track). Nothing is remapped silently.
#pragma once

#include <memory>

#include "ui/AppContext.h"
#include "vcc/notation/ExpressionMap.h"

namespace vcc::desktop {

class ArticulationMapPanel final : public juce::Component, private juce::Timer {
public:
    ArticulationMapPanel(AppContext& context, std::string track_id);
    ~ArticulationMapPanel() override;

    /// The map as edited (not yet applied).
    const notation::ExpressionMap& map() const noexcept { return map_; }
    /// Starts from a built-in ("default", "orchestral_cc") or the track's.
    void start_from(const std::string& builtin);
    /// Row 0 is the natural switch; then one row per articulation.
    int rows() const;
    juce::String row_name(int row) const;
    void set_action(int row, int action_index);  ///< 0 = none, then kActions order.
    void set_values(int row, int a, int b);
    /// Learns the row's switch from the next MIDI message.
    void learn(int row);
    int learning_row() const noexcept { return learning_; }
    bool apply();
    /// Applies, and stores the map in the track's saved library patch.
    bool save_to_patch();
    juce::String health() const;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    class Row;
    void timerCallback() override;
    void load_rows();
    void rule_changed(int row, const notation::ArticulationRule* rule);

    AppContext& context_;
    std::string track_;
    notation::ExpressionMap map_;
    juce::Label title_, health_, status_;
    juce::ComboBox base_, dynamics_;
    juce::Slider dynamics_cc_{juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft};
    juce::TextEditor name_;
    juce::Viewport view_;
    juce::Component list_;
    std::vector<std::unique_ptr<Row>> rows_;
    juce::TextButton apply_{"Apply"}, save_patch_{"Apply + save to patch"}, revert_{"Revert"};
    int learning_ = -1;
};

void show_articulation_map(AppContext& context, const std::string& track_id);
ArticulationMapPanel* articulation_map_panel();

/// Tracks whose instrument plug-in is not installed here (name, track id).
std::vector<std::pair<std::string, std::string>> missing_instruments(AppContext& context);
/// Opens the Missing Instruments window when there are any; returns how many.
int show_missing_instruments(AppContext& context);
juce::Component* missing_instruments_panel();

}  // namespace vcc::desktop
