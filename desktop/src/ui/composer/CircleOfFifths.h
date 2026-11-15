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
#include "vcc/theory/Progression.h"

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
    /// Shows or hides the borrowed chords (modal mixture). Remembered.
    void set_borrowed(bool on);
    bool borrowed_shown() const noexcept { return borrowed_; }
    /// The borrowed chords shown (triads or sevenths, as the 7ths key says),
    /// from the source chosen: the parallel key first, or another scale on
    /// the same tonic. Remembered.
    const std::vector<theory::BorrowedChord>& borrowed() const noexcept { return lent_; }
    void set_borrow_source(theory::BorrowSource source);
    theory::BorrowSource borrow_source() const noexcept { return borrow_source_; }
    juce::ComboBox& borrow_box() { return borrow_from_; }
    /// Selects a borrowed chord: highlighted, heard, ready for Chord Mode.
    void select_borrowed(int index, bool audition = true);
    int selected_borrowed() const noexcept { return selected_borrowed_; }
    /// The selected chord's line: "bVI = Ab (Ab C Eb), from C minor - Ab and Eb are outside C major".
    juce::String selection_text() const;

    // --- Progression ---------------------------------------------------------------------
    /// Clicking chords builds a progression in the order clicked (each key
    /// shows its place: 1, 2, 3...); clicking one again takes it out (the
    /// rest renumber); Shift-click adds it again (a repeat). Changing key
    /// clears it - its numerals belong to the key it was built in.
    struct Step {
        theory::Chord chord;
        std::string roman;
        bool borrowed = false;
        std::string source;  ///< Borrowed: where from.
    };
    const std::vector<Step>& progression() const noexcept { return progression_; }
    /// Places of a key's chord in the progression (1-based), for its badge.
    std::vector<int> places(bool borrowed, int index) const;
    /// Toggles a diatonic (or borrowed) chord in the progression, or with
    /// `repeat` adds it again.
    void toggle_step(bool borrowed, int index, bool repeat = false);
    void clear_progression();
    /// Each chord's length when played or put on a track: 0 two beats, 1 a bar, 2 two bars.
    void set_step_length(int which);
    int step_length() const noexcept { return step_length_; }
    /// Plays the progression in order (each chord lit as it sounds).
    void play_progression();
    void stop_progression(bool send_note_offs = true);
    bool playing_progression() const noexcept { return player_ != nullptr; }
    int playing_step() const noexcept { return playing_step_; }
    /// Writes it on the selected track at the playhead (on the beat), with
    /// its chord symbols over it, as one undo step (asks before overwriting):
    /// as notation in the score (spelled as the chords are, split at
    /// barlines and tied) or as MIDI, as "Put as" says.
    bool put_on_track(project::CollisionPolicy policy = project::CollisionPolicy::Ask);
    /// Notation (true, the default) or MIDI.
    void set_put_as_notation(bool on);
    bool put_as_notation() const noexcept { return as_notation_; }
    /// Voicing: 0 root in the bass with smooth upper voices, 1 smooth with
    /// free inversions, 2 root position (stacked).
    void set_voicing(int which);
    int voicing() const noexcept { return voicing_; }
    /// Rhythm: block, pulse on the beat, arpeggio in eighths, strum.
    void set_rhythm(theory::ChordRhythm rhythm);
    theory::ChordRhythm rhythm() const noexcept { return rhythm_; }
    /// The progression as it plays and is written from `start`.
    theory::ProgressionRender render_from(Ticks start) const;

    /// My progressions: saved by name (with their key, lengths, voicing
    /// and rhythm), for any project; loading one shows its key.
    bool save_progression(const juce::String& name);
    bool load_progression(const juce::String& name);
    bool delete_progression(const juce::String& name);
    juce::StringArray saved_progressions() const;
    /// "1 I G - 2 vi Em - 3 bVI Eb".
    juce::String progression_text() const;
    hw::Keycap& play_key() { return audition_; }
    hw::Keycap& put_key() { return put_; }
    hw::Keycap& clear_key() { return clear_; }
    /// Fills the Composer's chord palette with the selected chord; inserts nothing.
    bool send_to_chord_mode();
    /// Asks, then sets the project's key to the one shown (one undo step).
    void apply_key_to_score();
    /// Re-reads the score key (follow mode) and redraws.
    void refresh();

    /// Where a segment is (for checks and hit testing).
    juce::Point<float> segment_centre(int index, bool minor) const;

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;

    // For the checks.
    hw::Keycap& follow_key() { return follow_key_; }
    hw::Keycap& manual_key() { return manual_key_; }
    hw::Keycap& chord_key(int i) { return *chord_keys_[static_cast<std::size_t>(i)]; }
    hw::Keycap& send_key() { return send_; }
    hw::Keycap& borrowed_toggle() { return borrowed_key_; }
    hw::Keycap& borrowed_key(int i) { return *borrowed_keys_[static_cast<std::size_t>(i)]; }
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
    int selected_borrowed_ = -1;
    bool borrowed_ = true;
    theory::BorrowSource borrow_source_ = theory::BorrowSource::Parallel;
    std::vector<theory::BorrowedChord> lent_;
    juce::ComboBox borrow_from_;
    std::vector<Step> progression_;
    int step_length_ = 1;
    int playing_step_ = -1;
    struct Player;
    std::unique_ptr<Player> player_;
    std::string progression_key_;
    hw::Keycap put_{"Put on track"}, clear_{"Clear"}, save_{"Save..."};
    juce::ComboBox length_box_, output_box_, voicing_box_, rhythm_box_, saved_box_;
    bool as_notation_ = true;
    int voicing_ = 0;
    theory::ChordRhythm rhythm_ = theory::ChordRhythm::Block;
    void refresh_saved();
    Ticks step_ticks(Ticks at) const;
    std::vector<int> voiced(const theory::Chord& chord) const;
    /// The chord selected (diatonic or borrowed), or null.
    const theory::Chord* selected_chord_ptr() const;
    void audition(const theory::Chord& chord);
    juce::Rectangle<int> chords_area() const;
    theory::KeyReference ref_;
    juce::String anchor_;
    std::string last_key_;

    hw::Keycap follow_key_{"Follow score key", false}, manual_key_{"Manual", false}, sevenths_key_{"7ths", true};
    hw::Keycap apply_{"Apply key to score..."}, send_{"Send to Chord Mode"}, audition_{"Audition"};
    juce::ComboBox form_;
    std::vector<std::unique_ptr<hw::Keycap>> chord_keys_;
    std::vector<std::unique_ptr<hw::Keycap>> relation_keys_;
    hw::Keycap borrowed_key_{"Borrowed chords", true};
    std::vector<std::unique_ptr<hw::Keycap>> borrowed_keys_;
};

/// Opens the Circle of Fifths window (or brings it to the front).
void show_circle_of_fifths(AppContext& context);
/// The open window's panel, or null (for checks).
CircleOfFifthsPanel* circle_of_fifths_panel();

}  // namespace vcc::desktop::composer_ui
