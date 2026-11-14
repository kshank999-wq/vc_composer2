// The Composer workspace (Main Spec §7; docs/spec/ui/SPEC.md, "Composer
// view"): the overview strip and the four ruler rows, then one row per
// instrument track - the 220 px track header and the lane with its staves
// and phrase blocks - aligned bar for bar, with notation entry on the
// selected track through the notation editor ported from the original VC
// Composer.
//
// The selected track is edited on a working copy of its score; each change is
// committed to the project as one undo step, so Undo, versions, the Producer
// view and playback all see the same notes (Main Spec §8).
#pragma once

#include <memory>
#include <optional>

#include "ui/AppContext.h"
#include "vcc/editing/ScoreEditor.h"
#include "vcc/ui/NotationView.h"

namespace vcc::desktop {

namespace composer_ui {
class ComposerRuler;
class TrackHeader;
class PhraseLayer;
}  // namespace composer_ui

class ComposerView final : public juce::Component,
                           private juce::ScrollBar::Listener,
                           private juce::ChangeListener,
                           private juce::Timer {
public:
    explicit ComposerView(AppContext& context);
    ~ComposerView() override;

    void set_playhead(Ticks tick, bool playing);
    bool note_entry() const noexcept { return note_entry_; }
    /// The notes selected in the score being edited (the shared selection's notes).
    std::vector<notation::EventId> selected_notes() const;
    /// The editor's cursor (value, dots, rest, voice), when a track is being written on.
    std::optional<editing::EditCursor> cursor() const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    /// Performs a shortcut-profile command on the selected track.
    void perform(composer::Command command);
    /// Step input from a key on the deck's keyboard: enters the sounding
    /// pitch `midi` at the cursor with the current value (as written for a
    /// transposing staff, spelt in its key), or adds it to the chord just
    /// entered. False when no instrument track is being written on.
    bool enter_midi(int midi, bool chord);
    /// Larger or smaller staves (the toolbar's Zoom keys).
    void zoom(bool larger);
    /// Scrolls so `tick` is the left edge of the lanes.
    void set_origin(Ticks tick);
    Ticks origin() const noexcept { return origin_; }

    // For the checks.
    composer_ui::ComposerRuler& ruler() { return *ruler_; }
    int strip_count() const noexcept { return static_cast<int>(strips_.size()); }
    composer_ui::TrackHeader& header(int i);
    composer_ui::PhraseLayer& phrases(int i);
    ui::NotationView& lane(int i);
    /// The x of `tick` in this component (the ruler's and lanes' shared timeline).
    float x_of_tick(Ticks tick);

private:
    class Strip;

    void rebuild();
    void reload_working_copy(bool new_editor);
    void apply_view_settings();
    void apply_colours();
    void align_clef_zones();
    void handle(const editing::EditResult& result);
    void ask_for_input(composer::Command command);
    void scrollBarMoved(juce::ScrollBar*, double start) override { set_origin(static_cast<Ticks>(start)); }
    void changeListenerCallback(juce::ChangeBroadcaster*) override { apply_colours(); }
    void timerCallback() override;
    void strip_clicked(Strip& strip, const engraving::InsertionPoint* point, notation::EventId id);
    Ticks visible_ticks() const;
    engraving::LayoutOptions lane_layout() const;

    AppContext& context_;
    int listener_ = 0;
    std::unique_ptr<composer_ui::ComposerRuler> ruler_;
    juce::Viewport viewport_;
    juce::Component strips_holder_;
    std::vector<std::unique_ptr<Strip>> strips_;
    juce::ScrollBar hscroll_{false};
    juce::Label hint_;

    notation::Score working_;
    std::unique_ptr<editing::ScoreEditor> editor_;
    std::string editing_track_;
    std::string editing_version_;
    bool note_entry_ = true;
    Ticks origin_ = 0;
    std::uint64_t generation_ = 0;
    /// The spec's 100%: a 7 px staff space and a 120 px bar of 4/4.
    static constexpr double kSpaceAt100 = 7.0;
    static constexpr double kSpacesPerQuarter = 30.0 / 7.0;
    double pixels_per_space_ = kSpaceAt100;
    bool committing_ = false;
    bool syncing_ = false;  ///< Exchanging the selection with the session.
    /// The selection is only the note just typed: a duration key then sets
    /// the next note's value (as in Finale and Sibelius) rather than
    /// re-timing that note.
    bool just_entered_ = false;
    /// Publishes the editor's selection to the session's shared selection.
    void publish_selection();
};

}  // namespace vcc::desktop
