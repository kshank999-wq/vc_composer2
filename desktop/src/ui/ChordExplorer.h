// Chord Explorer / Progression Builder (Templates Supplement §9): browse
// progressions, see them as chords in any key, audition chords and whole
// progressions, and send exactly 4/8 (or 12/24) bars to a track in one undo
// step. The preview key is local: it never changes the project key.
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <vector>

#include <juce_audio_utils/juce_audio_utils.h>

#include "ui/AppContext.h"
#include "ui/Dialogs.h"
#include "vcc/theory/KeyReference.h"

namespace vcc::desktop {

class ChordExplorer final : public juce::Component, public juce::DragAndDropContainer, private juce::ListBoxModel {
public:
    explicit ChordExplorer(AppContext& context);
    ~ChordExplorer() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // --- Your progression: bars (twelve to start, as many as you add), each
    // divided into 1-16 cells that hold a chord. Chords are dragged in from
    // the key's chords, the borrowed chords or a progression's cards, or
    // typed in a cell's editor, where its notes can be moved, added or
    // deleted. An empty cell holds the chord before it. Audition and Send
    // use the build while "My Build" is chosen (dropping a chord chooses it).
    static constexpr int kBuildBars = 12;      ///< Bars to start with.
    static constexpr int kMaxDivisions = 16;
    struct BuildChord {
        theory::Chord chord;
        std::string numeral;
        std::vector<int> notes;  ///< Its notes as edited; empty: voiced with the rest.
    };
    struct BuildBar {
        int divisions = 1;
        std::vector<std::optional<BuildChord>> cells = std::vector<std::optional<BuildChord>>(1);
    };
    int bar_count() const noexcept { return static_cast<int>(bars_.size()); }
    void add_bars(int n);
    /// Removes the last bar (never below one).
    void remove_last_bar();
    /// Divides a bar into `n` cells (1-16); chords keep their place where it still exists.
    void set_divisions(int bar, int n);
    int divisions(int bar) const { return bars_[static_cast<std::size_t>(bar)].divisions; }
    void set_cell(int bar, int cell, const theory::Chord& chord, const std::string& numeral);
    void clear_cell(int bar, int cell);
    const std::optional<BuildChord>& cell(int bar, int cell) const
    {
        return bars_[static_cast<std::size_t>(bar)].cells[static_cast<std::size_t>(cell)];
    }
    /// A cell's notes as edited (empty: back to voiced with the rest).
    void set_cell_notes(int bar, int cell, std::vector<int> notes);
    /// The notes a cell plays now (as edited, or as voiced).
    std::vector<int> cell_notes(int bar, int cell) const;
    /// Opens a cell's editor (chord symbol, notes).
    void edit_cell(int bar, int cell);
    /// Puts a chord in bar `slot` (0-based), first cell.
    void set_build_chord(int slot, const theory::Chord& chord, const std::string& numeral) { set_cell(slot, 0, chord, numeral); }
    /// Puts a chord in the first empty cell (adding a bar when all are full).
    bool add_to_build(const theory::Chord& chord, const std::string& numeral);
    void clear_build_chord(int slot);
    void clear_build();
    /// Copies the chosen progression's chords into the build, each bar divided as its chords fall.
    void fill_build_from_progression();
    const std::optional<BuildChord>& build_chord(int slot) const { return cell(slot, 0); }
    /// The build's zoom: bar width and strip height, 1 = normal.
    void set_build_zoom(double width, double height);
    /// Audition and Send use the build (true) or the chosen progression.
    void use_build(bool build);
    bool using_build() const noexcept { return use_build_; }
    /// The build voiced and laid out from the start bar, as Send writes it.
    std::optional<theory::ProgressionRender> render_build() const;
    /// The key's chords and the borrowed ones, as shown (for the checks).
    int borrowed_count() const noexcept { return static_cast<int>(borrowed_.size()); }
    const theory::Chord& borrowed_chord(int i) const;
    const theory::Chord& key_chord(int i) const;
    juce::ComboBox& borrow_box() { return borrow_from_; }
    void press_send() { send(); }
    /// The project bar the build's first bar lands on (the Start bar).
    int first_bar() const;
    /// Plays these notes as a chord (nothing is written).
    void audition_notes(const std::vector<int>& notes);

private:
    class MatrixButton;
    class Cards;
    class BuildStrip;
    class CellEditor;

    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged(int last_row) override;

    void filter();
    /// What Audition and Send use: the build's render or the progression's.
    const theory::ProgressionRender* active_render() const;
    theory::ProgressionRenderOptions render_options() const;
    void build_changed();
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
    juce::Label borrowed_caption_;
    juce::ComboBox borrow_from_;  ///< Where the borrowed chords come from (as in the Circle of Fifths).
    theory::BorrowSource borrow_source_ = theory::BorrowSource::Parallel;
    std::vector<std::unique_ptr<MatrixButton>> borrowed_;
    std::vector<BuildBar> bars_ = std::vector<BuildBar>(kBuildBars);
    std::unique_ptr<BuildStrip> strip_;
    juce::Viewport strip_view_;
    juce::TextButton use_preset_{"Preset"}, use_build_key_{"My Build"}, fill_{"Fill from Preset"}, clear_{"Clear"};
    juce::TextButton add_bars_{"+ 4 Bars"}, remove_bar_{"- Bar"};
    juce::Slider zoom_w_, zoom_h_;
    juce::Label zoom_w_caption_, zoom_h_caption_;
    double zoom_width_ = 1.0, zoom_height_ = 1.0;
    /// Rendered chord i came from (bar, cell): for the editor's voiced notes.
    std::vector<std::pair<int, int>> build_cells_;
    int strip_height() const;
    bool use_build_ = false;
    std::unique_ptr<Cards> cards_;
    juce::Label chord_info_;
    juce::MidiKeyboardState keyboard_state_;
    juce::MidiKeyboardComponent keyboard_{keyboard_state_, juce::MidiKeyboardComponent::horizontalKeyboard};
    juce::TextButton audition_{"Audition"}, stop_{"Stop"}, send_{"Send to Track"};
    dialogs::TrackSelector target_;

    std::vector<const theory::Progression*> shown_;
    std::optional<theory::ProgressionRender> render_;
    std::optional<theory::ProgressionRender> build_render_;
    juce::String error_;
    int chord_size_ = 3;
};

}  // namespace vcc::desktop
