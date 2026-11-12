// Chord Explorer / Progression Builder (Templates Supplement §9): browse
// progressions, see them as chords in any key, audition chords and whole
// progressions, and send exactly 4/8 (or 12/24) bars to a track in one undo
// step. The preview key is local: it never changes the project key.
#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <juce_audio_utils/juce_audio_utils.h>

#include "ui/AppContext.h"
#include "ui/Dialogs.h"

namespace vcc::desktop {

class ChordExplorer final : public juce::Component, private juce::ListBoxModel {
public:
    explicit ChordExplorer(AppContext& context);
    ~ChordExplorer() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class MatrixButton;
    class Cards;

    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged(int last_row) override;

    void filter();
    const theory::Progression* chosen() const;
    theory::Key key() const;
    theory::VoicingOptions voicing_options() const;
    void progression_changed();
    void rebuild_matrix();
    void rerender();
    void show_on_keyboard(const std::vector<int>& notes, const juce::String& caption);
    static juce::String describe(const std::string& numeral, const theory::Chord& chord, const std::vector<int>& notes);
    void audition_chord(const theory::Chord& chord);
    void audition_card(int index);
    void audition();
    void send();
    void send_with(const std::string& track_id, const theory::ProgressionRender& render, const std::string& prog_id,
                   const juce::String& what, project::CollisionPolicy policy);

    AppContext& context_;
    juce::Label key_caption_, length_caption_, start_caption_, voicing_caption_, rhythm_caption_, target_caption_;
    juce::ComboBox tonic_, mode_, length_, voicing_, rhythm_;
    juce::Slider start_;
    juce::ToggleButton voice_lead_{"Voice leading"};
    juce::TextEditor search_;
    juce::ListBox list_;
    juce::Label matrix_caption_;
    juce::TextButton triads_{"Triads"}, sevenths_{"7ths"}, ninths_{"9ths"};
    std::vector<std::unique_ptr<MatrixButton>> matrix_;
    std::unique_ptr<Cards> cards_;
    juce::Label chord_info_;
    juce::MidiKeyboardState keyboard_state_;
    juce::MidiKeyboardComponent keyboard_{keyboard_state_, juce::MidiKeyboardComponent::horizontalKeyboard};
    juce::TextButton audition_{"Audition"}, stop_{"Stop"}, send_{"Send to Track"};
    dialogs::TrackSelector target_;

    std::vector<const theory::Progression*> shown_;
    std::optional<theory::ProgressionRender> render_;
    juce::String error_;
    int chord_size_ = 3;
};

}  // namespace vcc::desktop
