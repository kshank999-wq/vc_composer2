// Copyright (c) VC Composer. All rights reserved.
//
// The notation strip you can type into (spec 02 sections 5.2, 8, 11).
//
// This is the join: a NotationView draws the score and says what was
// clicked, a ShortcutProfile says what a keystroke means, and a ScoreEditor
// performs it. None of those three knows about the others, which is why
// each is testable on its own - and why this component is small.
//
// Pitch letters are handled here rather than in the editor because they are
// not commands: A through G name notes, and only while note entry is on.
// Spec 5.2's "toggle keyboard entry mode" is what turns that on and off,
// and the editor deliberately reports that command as not its own.
#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "vcc/composer/ShortcutProfile.h"
#include "vcc/editing/ScoreEditor.h"
#include "vcc/ui/KeyMapping.h"
#include "vcc/ui/NotationView.h"

namespace vcc::ui {

class NotationEditorView : public juce::Component {
public:
    NotationEditorView();
    ~NotationEditorView() override;

    /// The score to show and edit, with an editor made for it. Not owned.
    /// Passing null detaches, so the view shows nothing and keystrokes do
    /// nothing.
    void set_score(notation::Score* score);

    /// Shows a score that already has an editor - a document's, whose undo
    /// history belongs to that document rather than to this view. Neither
    /// is owned, and both must outlive the view.
    ///
    /// A window with several parts open needs this: making an editor per
    /// view would give each view its own undo history for a score the
    /// document already has one for.
    void set_document(notation::Score* score, editing::ScoreEditor* editor);

    void set_profile(composer::ShortcutProfile profile);
    const composer::ShortcutProfile& profile() const noexcept { return profile_; }

    /// Note entry: letter keys name pitches while it is on (spec 5.2).
    void set_note_entry(bool entering);
    bool note_entry() const noexcept { return note_entry_; }

    NotationView& view() noexcept { return view_; }
    const NotationView& view() const noexcept { return view_; }

    /// Null until a score is set.
    editing::ScoreEditor* editor() noexcept { return editor_; }

    /// Every edit, for a status line. Reports refusals and commands that
    /// belong elsewhere too, since those are the ones a user needs told.
    std::function<void(const editing::EditResult&)> on_edit;

    /// Performs a keystroke as if it had been typed. The keyboard path and
    /// this share one implementation, so a test drives exactly what a user
    /// would.
    editing::EditResult perform(const composer::KeyStroke& stroke);

    // --- Component ---------------------------------------------------------
    void resized() override;
    bool keyPressed(const juce::KeyPress& press) override;

private:
    /// Pushes the editor's cursor and selection into the view and repaints.
    void refresh_from_editor();

    NotationView view_;
    composer::ShortcutProfile profile_;
    /// Owned only when this view made it; null when an editor was supplied.
    std::unique_ptr<editing::ScoreEditor> owned_editor_;
    editing::ScoreEditor* editor_ = nullptr;
    notation::Score* score_ = nullptr;
    bool note_entry_ = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NotationEditorView)
};

}  // namespace vcc::ui
