// The Circle of Fifths and key reference window (Add-On Features spec §9):
// a floating, resizable reference that stays open while notes are entered.
// It follows the score's key (at the selection, else the playhead) or a key
// chosen on the circle, and never changes the score unless "Apply key to
// score" is confirmed. Everything it shows comes from theory::key_reference.
#pragma once

#include <memory>
#include <vector>

#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"
#include "vcc/theory/KeyReference.h"

namespace vcc::desktop::composer_ui {

class CircleOfFifthsPanel final : public juce::Component, private juce::Timer {
public:
    explicit CircleOfFifthsPanel(AppContext& context);
    ~CircleOfFifthsPanel() override;

    /// Follow Score Key (true) or Manual Reference (false). Remembered.
    bool following() const noexcept { return follow_; }
    void set_following(bool on);
    /// Manual Reference: shows `key` (and leaves follow mode).
    void set_reference(const theory::Key& key);
    /// A click on a segment: `index` 0-11 clockwise from C, the inner ring
    /// for its minor key.
    void choose_position(int index, bool minor);
    /// The key shown, and what it is built from.
    const theory::KeyReference& reference() const noexcept { return ref_; }
    const juce::String& anchor_text() const noexcept { return anchor_; }
    void set_minor_form(theory::MinorForm form);
    void set_sevenths(bool on);
    /// Selects a diatonic chord (0-6): its notes are highlighted and heard.
    void select_chord(int index, bool audition = true);
    int selected_chord() const noexcept { return selected_; }
    /// Fills the Composer's chord palette with the selected chord; inserts nothing.
    bool send_to_chord_mode();
    /// Asks, then sets the project's key to the one shown (one undo step).
    void apply_key_to_score();
    /// Re-reads the score key (follow mode) and redraws.
    void refresh();

    /// Where a segment is (for checks and hit testing).
    juce::Point<float> segment_centre(int index, bool minor) const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;

    // For the checks.
    hw::Keycap& follow_key() { return follow_key_; }
    hw::Keycap& manual_key() { return manual_key_; }
    hw::Keycap& chord_key(int i) { return *chord_keys_[static_cast<std::size_t>(i)]; }
    hw::Keycap& send_key() { return send_; }
    juce::ComboBox& form_box() { return form_; }

private:
    void timerCallback() override { refresh(); }
    void rebuild();
    void remember() const;
    theory::Key score_key(juce::String& anchor) const;
    juce::Rectangle<float> circle_area() const;
    juce::Rectangle<int> info_area() const;
    std::pair<int, bool> position_at(juce::Point<float> p) const;

    AppContext& context_;
    bool follow_ = true;
    theory::Key manual_;
    theory::MinorForm form_value_ = theory::MinorForm::Natural;
    bool sevenths_ = false;
    int selected_ = -1;
    theory::KeyReference ref_;
    juce::String anchor_;
    std::string last_key_;

    hw::Keycap follow_key_{"Follow score key", false}, manual_key_{"Manual", false}, sevenths_key_{"7ths", true};
    hw::Keycap apply_{"Apply key to score..."}, send_{"Send to Chord Mode"}, audition_{"Audition"};
    juce::ComboBox form_;
    std::vector<std::unique_ptr<hw::Keycap>> chord_keys_;
    std::vector<std::unique_ptr<hw::Keycap>> relation_keys_;
};

/// Opens the Circle of Fifths window (or brings it to the front).
void show_circle_of_fifths(AppContext& context);
/// The open window's panel, or null (for checks).
CircleOfFifthsPanel* circle_of_fifths_panel();

}  // namespace vcc::desktop::composer_ui
