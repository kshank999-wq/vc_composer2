#include "vcc/editing/EditHistory.h"

#include <algorithm>

namespace vcc::editing {
namespace {

const std::string& empty_name()
{
    static const std::string empty;
    return empty;
}

bool apply_note(notation::Score& score, const EditStep& step, bool forward)
{
    const bool want_before = !forward;

    // Insertion undone, or deletion redone: the note goes away.
    if (want_before ? !step.had_before : !step.has_after) {
        const notation::EventId id = want_before ? step.note_after.id : step.note_before.id;
        return score.remove_note(id);
    }

    const notation::ScoreNote& target = want_before ? step.note_before : step.note_after;

    // Deletion undone, or insertion redone: the note comes back with the id
    // it had. Not a fresh insert - see the header.
    if (want_before ? !step.has_after : !step.had_before)
        return score.restore_note(step.staff_index, target);

    return score.modify_note(target.id, target);
}

bool apply_dynamic(notation::Score& score, const EditStep& step, bool forward)
{
    const bool want_before = !forward;
    const bool want_present = want_before ? step.had_before : step.has_after;
    const notation::DynamicMarking& target =
        want_before ? step.dynamic_before : step.dynamic_after;
    const notation::DynamicMarking& other =
        want_before ? step.dynamic_after : step.dynamic_before;

    // Remove whatever is at the position this step owns, then put back what
    // should be there. Doing both unconditionally keeps replacement,
    // insertion and deletion on one path.
    score.remove_dynamic(want_present ? target.tick : other.tick,
                         want_present ? target.voice : other.voice,
                         want_present ? target.staff : other.staff);
    if (!want_present)
        return true;

    score.add_dynamic(target);
    return true;
}

bool apply_hairpin(notation::Score& score, const EditStep& step, bool forward)
{
    const bool want_before = !forward;
    const bool want_present = want_before ? step.had_before : step.has_after;
    const notation::Hairpin& target = want_before ? step.hairpin_before : step.hairpin_after;
    const notation::Hairpin& other = want_before ? step.hairpin_after : step.hairpin_before;

    score.remove_hairpin(want_present ? target.start_tick : other.start_tick,
                         want_present ? target.voice : other.voice,
                         want_present ? target.staff : other.staff);
    if (!want_present)
        return true;

    score.add_hairpin(target);
    return true;
}

bool apply_slur(notation::Score& score, const EditStep& step, bool forward)
{
    const bool want_before = !forward;
    const bool want_present = want_before ? step.had_before : step.has_after;
    const notation::Slur& target = want_before ? step.slur_before : step.slur_after;
    const notation::Slur& other = want_before ? step.slur_after : step.slur_before;

    if (want_before ? step.has_after : step.had_before)
        score.remove_slur(other);
    if (!want_present)
        return true;

    score.add_slur(target);
    return true;
}

bool apply_time_signature(notation::Score& score, const EditStep& step, bool forward)
{
    const bool want_before = !forward;
    const bool want_present = want_before ? step.had_before : step.has_after;

    if (!want_present)
        return score.remove_time_signature(step.signature_tick);
    return score.set_time_signature(step.signature_tick,
                                    want_before ? step.meter_before : step.meter_after);
}

bool apply_key_signature(notation::Score& score, const EditStep& step, bool forward)
{
    const bool want_before = !forward;
    const bool want_present = want_before ? step.had_before : step.has_after;

    if (!want_present)
        return score.remove_key_signature(step.signature_tick);
    return score.set_key_signature(step.signature_tick,
                                   want_before ? step.key_before : step.key_after);
}

bool apply_clef(notation::Score& score, const EditStep& step, bool forward)
{
    if (step.staff_index >= score.staff_count())
        return false;
    notation::Staff changed = score.staff(step.staff_index);
    changed.clef = forward ? step.clef_after : step.clef_before;
    // Through set_staff, so a clef change counts as a change: written
    // through staff() it did not, and a part with a new clef looked saved.
    return score.set_staff(step.staff_index, changed);
}

bool apply_staff(notation::Score& score, const EditStep& step, bool forward)
{
    const bool want_before = !forward;
    const bool want_present = want_before ? step.had_before : step.has_after;
    const bool was_present = want_before ? step.has_after : step.had_before;
    const notation::Staff& wanted = want_before ? step.staff_before : step.staff_after;

    if (want_present && was_present) {
        // How the staff reads, not whether it exists.
        return score.set_staff(step.staff_index, wanted);
    }
    if (want_present) {
        // Added at the bottom, which is the only place a staff is added.
        if (step.staff_index != score.staff_count())
            return false;
        return score.add_staff(wanted) == step.staff_index;
    }
    // Taken away from the bottom, which the score refuses while it has
    // music - so a step that got this far is reversible.
    if (step.staff_index + 1 != score.staff_count())
        return false;
    return score.remove_staff(step.staff_index);
}

bool apply_text(notation::Score& score, const EditStep& step, bool forward)
{
    const bool want_before = !forward;
    const bool want_present = want_before ? step.had_before : step.has_after;
    const notation::TextMarking& target = want_before ? step.text_before : step.text_after;
    const notation::TextMarking& other = want_before ? step.text_after : step.text_before;

    score.remove_text(want_present ? target.tick : other.tick,
                      want_present ? target.voice : other.voice);
    if (!want_present)
        return true;

    score.add_text(target);
    return true;
}

}  // namespace

bool apply_step(notation::Score& score, const EditStep& step, bool forward)
{
    switch (step.target) {
        case EditStep::Target::Note:          return apply_note(score, step, forward);
        case EditStep::Target::Dynamic:       return apply_dynamic(score, step, forward);
        case EditStep::Target::Hairpin:       return apply_hairpin(score, step, forward);
        case EditStep::Target::Slur:          return apply_slur(score, step, forward);
        case EditStep::Target::TimeSignature: return apply_time_signature(score, step, forward);
        case EditStep::Target::KeySignature:  return apply_key_signature(score, step, forward);
        case EditStep::Target::Clef:          return apply_clef(score, step, forward);
        case EditStep::Target::Text:          return apply_text(score, step, forward);
        case EditStep::Target::Staff:         return apply_staff(score, step, forward);
    }
    return false;
}

EditHistory::EditHistory(std::size_t depth) : capacity_(std::max<std::size_t>(1, depth)) {}

void EditHistory::begin(const std::string& name, const EditCursor& cursor_before)
{
    // An open transaction here means an edit gave up part way through. Its
    // steps were already applied to the score, so this loses the ability to
    // undo them - but keeping them would make undo replay half an edit,
    // which is worse.
    open_ = Transaction{};
    open_.name = name;
    open_.cursor_before = cursor_before;
    recording_ = true;
}

void EditHistory::record(const EditStep& step)
{
    if (!recording_)
        return;
    open_.steps.push_back(step);
}

bool EditHistory::commit(const EditCursor& cursor_after)
{
    if (!recording_)
        return false;

    recording_ = false;
    if (open_.steps.empty()) {
        open_ = Transaction{};
        return false;
    }

    open_.cursor_after = cursor_after;
    push(std::move(open_));
    open_ = Transaction{};
    return true;
}

void EditHistory::abandon()
{
    recording_ = false;
    open_ = Transaction{};
}

bool EditHistory::rollback(notation::Score& score)
{
    bool all_reversed = true;
    for (std::size_t index = open_.steps.size(); index > 0; --index)
        all_reversed = apply_step(score, open_.steps[index - 1], false) && all_reversed;

    abandon();
    return all_reversed;
}

void EditHistory::push(Transaction transaction)
{
    undo_.push_back(std::move(transaction));

    // A new edit makes the redo stack meaningless: it described a future
    // that no longer follows from this state.
    redo_.clear();

    if (undo_.size() > capacity_)
        undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(
                                                       undo_.size() - capacity_));
}

const std::string& EditHistory::undo_name() const noexcept
{
    return undo_.empty() ? empty_name() : undo_.back().name;
}

const std::string& EditHistory::redo_name() const noexcept
{
    return redo_.empty() ? empty_name() : redo_.back().name;
}

bool EditHistory::undo(notation::Score& score, EditCursor& cursor)
{
    if (undo_.empty())
        return false;

    Transaction transaction = std::move(undo_.back());
    undo_.pop_back();

    // Backwards, so a transaction that removed a note and then inserted
    // another in its place undoes the insert first and the removal second.
    bool all_applied = true;
    for (std::size_t index = transaction.steps.size(); index > 0; --index)
        all_applied = apply_step(score, transaction.steps[index - 1], false) && all_applied;

    cursor = transaction.cursor_before;
    redo_.push_back(std::move(transaction));
    return all_applied;
}

bool EditHistory::redo(notation::Score& score, EditCursor& cursor)
{
    if (redo_.empty())
        return false;

    Transaction transaction = std::move(redo_.back());
    redo_.pop_back();

    bool all_applied = true;
    for (const EditStep& step : transaction.steps)
        all_applied = apply_step(score, step, true) && all_applied;

    cursor = transaction.cursor_after;
    undo_.push_back(std::move(transaction));
    return all_applied;
}

void EditHistory::clear()
{
    undo_.clear();
    redo_.clear();
    abandon();
}

}  // namespace vcc::editing
