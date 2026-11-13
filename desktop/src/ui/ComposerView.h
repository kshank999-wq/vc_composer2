// The Composer workspace (Main Spec §7): every instrument track's staves on a
// shared timeline, aligned bar for bar, with notation entry on the selected
// track through the notation editor ported from the original VC Composer.
//
// The selected track is edited on a working copy of its score; each change is
// committed to the project as one undo step, so Undo, versions, the Sequencer
// and playback all see the same notes (Main Spec §8).
#pragma once

#include <memory>

#include "ui/AppContext.h"
#include "vcc/editing/ScoreEditor.h"
#include "vcc/ui/NotationView.h"

namespace vcc::desktop {

class ComposerView final : public juce::Component, private juce::ScrollBar::Listener {
public:
    explicit ComposerView(AppContext& context);
    ~ComposerView() override;

    void set_playhead(Ticks tick, bool playing);
    bool note_entry() const noexcept { return note_entry_; }
    /// The notes selected in the score being edited (the shared selection's notes).
    std::vector<notation::EventId> selected_notes() const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    /// Performs a shortcut-profile command on the selected track.
    void perform(composer::Command command);

private:
    class Strip;
    class Palette;

    void rebuild();
    void reload_working_copy(bool new_editor);
    void apply_view_settings();
    void handle(const editing::EditResult& result);
    void ask_for_input(composer::Command command);
    void set_origin(Ticks tick);
    void scrollBarMoved(juce::ScrollBar*, double start) override { set_origin(static_cast<Ticks>(start)); }

public:
    /// Larger or smaller staves (the toolbar's Zoom keys).
    void zoom(bool larger);

private:
    void strip_clicked(Strip& strip, const engraving::InsertionPoint* point, notation::EventId id);
    Ticks visible_ticks() const;

    AppContext& context_;
    int listener_ = 0;
    std::unique_ptr<Palette> palette_;
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
    double spaces_per_quarter_ = ui::NotationView::kDefaultSpacesPerQuarter;
    double pixels_per_space_ = 8.0;
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
