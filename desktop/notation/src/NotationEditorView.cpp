#include "vcc/ui/NotationEditorView.h"

namespace vcc::ui {
namespace {

editing::EditResult unhandled(const char* message)
{
    editing::EditResult result;
    result.status = editing::EditStatus::NotHandled;
    result.message = message;
    return result;
}

}  // namespace

NotationEditorView::NotationEditorView() : profile_(composer::make_vc_composer_profile())
{
    addAndMakeVisible(view_);
    setWantsKeyboardFocus(true);

    view_.on_event_clicked = [this](notation::EventId id) {
        if (editor_ == nullptr)
            return;
        // Selecting a note also takes the cursor to its staff and voice, so
        // what is typed next goes where the composer just pointed.
        editor_->select_and_follow(id);
        refresh_from_editor();
    };

    view_.on_staff_clicked = [this](const engraving::InsertionPoint& insertion) {
        if (editor_ == nullptr || !insertion.valid)
            return;

        // Mouse entry (spec 5.2): the click says where and which pitch, the
        // editor's current duration says what kind of note.
        const editing::EditResult result =
            editor_->insert_at(insertion.staff_index, insertion.tick, insertion.pitch);
        refresh_from_editor();
        if (on_edit)
            on_edit(result);
    };
}

NotationEditorView::~NotationEditorView() = default;

void NotationEditorView::set_score(notation::Score* score)
{
    score_ = score;
    owned_editor_ = score != nullptr ? std::make_unique<editing::ScoreEditor>(*score) : nullptr;
    editor_ = owned_editor_.get();
    view_.set_score(score);
    refresh_from_editor();
}

void NotationEditorView::set_document(notation::Score* score, editing::ScoreEditor* editor)
{
    // The document owns both, so this view owns neither - and drops any
    // editor it made earlier, whose history belonged to a score it is no
    // longer showing.
    owned_editor_.reset();
    score_ = score;
    editor_ = score != nullptr ? editor : nullptr;
    view_.set_score(score);
    refresh_from_editor();
}

void NotationEditorView::set_profile(composer::ShortcutProfile profile)
{
    profile_ = std::move(profile);
}

void NotationEditorView::set_note_entry(bool entering)
{
    note_entry_ = entering;
}

void NotationEditorView::resized()
{
    view_.setBounds(getLocalBounds());
}

void NotationEditorView::refresh_from_editor()
{
    if (editor_ == nullptr) {
        view_.set_selection({});
        return;
    }

    view_.set_selection(editor_->selection());
    view_.set_cursor(editor_->cursor().tick, editor_->cursor().staff_index, note_entry_);
    view_.score_changed();
}

editing::EditResult NotationEditorView::perform(const composer::KeyStroke& stroke)
{
    if (editor_ == nullptr)
        return unhandled("No score to edit");

    // In note entry, an unmodified letter names a pitch rather than
    // triggering whatever is bound to that letter. That is how every
    // notation program behaves, and without it a profile that binds a
    // letter would make that note untypeable.
    notation::Step step = notation::Step::C;
    if (note_entry_ && stroke.modifiers == 0 && step_from_key(stroke, step)) {
        const editing::EditResult result = editor_->insert_step(step);
        refresh_from_editor();
        if (on_edit)
            on_edit(result);
        return result;
    }

    // Shift and a letter adds that pitch to the note just entered, making
    // a chord - after a grace note, a grace chord. A profile cannot bind
    // Shift and a pitch letter to anything else in note entry, for the
    // same reason as the bare letter.
    if (note_entry_ && stroke.modifiers == composer::to_mask(composer::Modifier::Shift)
        && step_from_key(stroke, step)) {
        const editing::EditResult result = editor_->add_step_to_chord(step);
        refresh_from_editor();
        if (on_edit)
            on_edit(result);
        return result;
    }

    const composer::Command command = profile_.command_for(stroke);
    if (command == composer::Command::None)
        return unhandled("Nothing is bound to that key");

    // The editor reports this one as not its own, because entry mode is a
    // property of the surface the typing goes into rather than of the score.
    if (command == composer::Command::ToggleKeyboardEntryMode) {
        set_note_entry(!note_entry_);
        refresh_from_editor();

        editing::EditResult result;
        result.status = editing::EditStatus::Applied;
        result.message = note_entry_ ? "Note entry on" : "Note entry off";
        if (on_edit)
            on_edit(result);
        return result;
    }

    const editing::EditResult result = editor_->apply(command);
    refresh_from_editor();
    if (on_edit)
        on_edit(result);
    return result;
}

bool NotationEditorView::keyPressed(const juce::KeyPress& press)
{
    composer::KeyStroke stroke;
    if (!key_stroke_from(press, stroke))
        return false;

    const editing::EditResult result = perform(stroke);

    // Returning false lets an unbound key travel on to the rest of the
    // application, which is what a key the notation view has no use for
    // should do. A command the editor answers with "needs input" - the
    // key, the meter, the clef, text - travels on too: the editor changed
    // nothing, and the window is what can ask.
    return result.status != editing::EditStatus::NotHandled
        && result.status != editing::EditStatus::NeedsInput;
}

}  // namespace vcc::ui
