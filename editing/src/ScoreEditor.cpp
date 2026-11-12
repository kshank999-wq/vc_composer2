#include "vcc/editing/ScoreEditor.h"

#include <algorithm>

namespace vcc::editing {
namespace {

EditResult applied(const char* message, std::size_t notes = 0,
                   notation::EventId event = notation::kInvalidEventId,
                   bool score_changed = true)
{
    EditResult result;
    result.status = EditStatus::Applied;
    result.message = message;
    result.notes_changed = notes;
    result.new_event = event;
    result.score_changed = score_changed;
    return result;
}

EditResult tools_applied(const char* message)
{
    return applied(message, 0, notation::kInvalidEventId, false);
}

EditResult nothing(const char* message)
{
    EditResult result;
    result.status = EditStatus::NothingToDo;
    result.message = message;
    return result;
}

EditResult refused(const char* message)
{
    EditResult result;
    result.status = EditStatus::Refused;
    result.message = message;
    return result;
}

EditResult not_handled(const char* message)
{
    EditResult result;
    result.status = EditStatus::NotHandled;
    result.message = message;
    return result;
}

EditResult needs_input(const char* message)
{
    EditResult result;
    result.status = EditStatus::NeedsInput;
    result.message = message;
    return result;
}

/// A pitch's alternative spelling of the same sounding note: F sharp as
/// G flat. Returns false when there is none inside a double accidental,
/// which is the case for the naturals in the middle of the staff.
bool enharmonic_of(const notation::Pitch& pitch, notation::Pitch& out)
{
    const int sounding = notation::midi_note_number(pitch);
    if (sounding < 0)
        return false;

    // Try the step above and the step below; a third spelling would need a
    // triple accidental, which the model does not represent.
    for (const int delta : {1, -1}) {
        const int diatonic = notation::diatonic_index(pitch) + delta;
        if (diatonic < 0)
            continue;

        notation::Pitch candidate;
        candidate.octave = static_cast<std::int8_t>(diatonic / 7);
        candidate.step = static_cast<notation::Step>(diatonic % 7);

        const int natural = notation::semitones_above_c(candidate.step)
                          + (static_cast<int>(candidate.octave) + 1) * 12;
        const int alteration = sounding - natural;
        if (alteration < -2 || alteration > 2)
            continue;

        candidate.alteration = static_cast<std::int8_t>(alteration);
        if (!candidate.is_valid() || candidate == pitch)
            continue;

        out = candidate;
        return true;
    }
    return false;
}

}  // namespace

const char* to_string(EditStatus status) noexcept
{
    switch (status) {
        case EditStatus::Applied:     return "applied";
        case EditStatus::NothingToDo: return "nothing to do";
        case EditStatus::NotHandled:  return "not handled here";
        case EditStatus::NeedsInput:  return "needs input";
        case EditStatus::Refused:     break;
    }
    return "refused";
}

void Clipboard::clear()
{
    notes.clear();
    span = 0;
}

ScoreEditor::ScoreEditor(notation::Score& score) : score_(&score) {}

// --- Cursor and selection ---------------------------------------------------

void ScoreEditor::set_cursor(const EditCursor& cursor)
{
    cursor_ = cursor;
    if (cursor_.staff_index >= score_->staff_count())
        cursor_.staff_index = score_->staff_count() - 1;
    if (cursor_.tick < 0)
        cursor_.tick = 0;
}

EditResult ScoreEditor::move_cursor(CursorStep step, int direction)
{
    if (direction == 0)
        return nothing("No direction to move in");

    switch (step) {
        case CursorStep::Duration: {
            const notation::Ticks length = cursor_.duration.ticks();
            if (length <= 0)
                return refused("The selected duration has no length");
            const notation::Ticks moved =
                cursor_.tick + static_cast<notation::Ticks>(direction) * length;
            if (moved < 0) {
                if (cursor_.tick == 0)
                    return nothing("Already at the start of the score");
                cursor_.tick = 0;
                return tools_applied("Moved to the start of the score");
            }
            cursor_.tick = moved;
            return tools_applied("Moved the cursor");
        }

        case CursorStep::Measure: {
            const notation::Measure current = score_->measure_at(cursor_.tick);
            if (direction > 0) {
                cursor_.tick = current.end_tick();
            } else if (cursor_.tick > current.start_tick) {
                // Mid-measure: go to this measure's start first, which is
                // what a musician means by "back a bar" from inside one.
                cursor_.tick = current.start_tick;
            } else if (current.number > 1) {
                cursor_.tick = score_->measure_start_tick(current.number - 1);
            } else {
                return nothing("Already at the first measure");
            }
            return tools_applied("Moved the cursor by a measure");
        }

        case CursorStep::Staff: {
            const int wanted = static_cast<int>(cursor_.staff_index) + direction;
            if (wanted < 0 || wanted >= static_cast<int>(score_->staff_count()))
                return nothing("No staff in that direction");
            cursor_.staff_index = static_cast<std::size_t>(wanted);
            return tools_applied("Moved to another staff");
        }
    }
    return nothing("Nothing to move");
}

void ScoreEditor::set_selection(std::vector<notation::EventId> selection)
{
    selection_ = std::move(selection);
}

void ScoreEditor::select(notation::EventId id)
{
    if (id == notation::kInvalidEventId)
        return;
    if (std::find(selection_.begin(), selection_.end(), id) == selection_.end())
        selection_.push_back(id);
}

void ScoreEditor::clear_selection()
{
    selection_.clear();
}

EditResult ScoreEditor::select_all()
{
    selection_.clear();
    for (std::size_t staff = 0; staff < score_->staff_count(); ++staff) {
        for (const notation::ScoreNote& note : score_->notes(staff))
            selection_.push_back(note.id);
    }
    if (selection_.empty())
        return nothing("The score is empty");
    return tools_applied("Selected everything");
}

std::vector<notation::EventId> ScoreEditor::notes_at_cursor() const
{
    std::vector<notation::EventId> found;
    if (cursor_.staff_index >= score_->staff_count())
        return found;

    for (const notation::ScoreNote& note : score_->notes(cursor_.staff_index)) {
        if (note.voice != cursor_.voice)
            continue;
        // A note is "at the cursor" if the cursor is anywhere inside it, so
        // a command works on the note being looked at rather than only on
        // one starting exactly there.
        if (note.start_tick <= cursor_.tick && note.end_tick() > cursor_.tick)
            found.push_back(note.id);
    }
    return found;
}

std::vector<notation::EventId> ScoreEditor::target_notes() const
{
    if (!selection_.empty())
        return selection_;
    return notes_at_cursor();
}

EditResult ScoreEditor::select_and_follow(notation::EventId id)
{
    const notation::ScoreNote* note = score_->find_note(id);
    if (note == nullptr)
        return nothing("There is no such note");

    selection_.assign(1, id);
    cursor_.staff_index = score_->staff_index_of(id);
    cursor_.voice = note->voice;
    return tools_applied("Selected a note");
}

// --- Voices -----------------------------------------------------------------

namespace {

/// The status line's text is a `const char*`, so each voice has its own.
const char* const kWritingInVoice[ScoreEditor::kVoiceCount] = {
    "Writing in voice 1", "Writing in voice 2", "Writing in voice 3", "Writing in voice 4"};
const char* const kAlreadyInVoice[ScoreEditor::kVoiceCount] = {
    "Already writing in voice 1", "Already writing in voice 2", "Already writing in voice 3",
    "Already writing in voice 4"};
const char* const kMovedToVoice[ScoreEditor::kVoiceCount] = {
    "Moved the selection to voice 1", "Moved the selection to voice 2",
    "Moved the selection to voice 3", "Moved the selection to voice 4"};
const char* const kSelectionInVoice[ScoreEditor::kVoiceCount] = {
    "The selection is already in voice 1", "The selection is already in voice 2",
    "The selection is already in voice 3", "The selection is already in voice 4"};
const char* const kMoveToVoiceName[ScoreEditor::kVoiceCount] = {
    "Move to voice 1", "Move to voice 2", "Move to voice 3", "Move to voice 4"};

}  // namespace

EditResult ScoreEditor::select_voice(std::uint8_t voice)
{
    if (voice >= kVoiceCount)
        return refused("A staff holds four voices");

    if (!selection_.empty()) {
        // Collected first: modifying re-sorts the staff, and the selection
        // is walked by id rather than by position for the same reason.
        std::vector<notation::ScoreNote> moving;
        for (const notation::EventId id : selection_) {
            const notation::ScoreNote* note = score_->find_note(id);
            if (note != nullptr && note->voice != voice)
                moving.push_back(*note);
        }

        if (!moving.empty()) {
            history_.begin(kMoveToVoiceName[voice], cursor_);
            for (const notation::ScoreNote& before : moving) {
                notation::ScoreNote after = before;
                after.voice = voice;
                modify_recorded(before, after);
            }
            // Entry carries on where the notes went.
            cursor_.voice = voice;
            history_.commit(cursor_);
            return applied(kMovedToVoice[voice], moving.size());
        }

        if (cursor_.voice == voice)
            return nothing(kSelectionInVoice[voice]);
    }

    if (cursor_.voice == voice)
        return nothing(kAlreadyInVoice[voice]);

    cursor_.voice = voice;
    return tools_applied(kWritingInVoice[voice]);
}

// --- Recording helpers ------------------------------------------------------

notation::EventId ScoreEditor::insert_recorded(std::size_t staff_index,
                                               const notation::ScoreNote& note)
{
    const notation::EventId id = score_->insert_note(staff_index, note);
    if (id == notation::kInvalidEventId)
        return id;

    EditStep step;
    step.target = EditStep::Target::Note;
    step.staff_index = staff_index;
    step.has_after = true;
    step.note_after = *score_->find_note(id);
    history_.record(step);
    return id;
}

bool ScoreEditor::remove_recorded(notation::EventId id)
{
    const notation::ScoreNote* existing = score_->find_note(id);
    if (existing == nullptr)
        return false;

    EditStep step;
    step.target = EditStep::Target::Note;
    step.staff_index = score_->staff_index_of(id);
    step.had_before = true;
    step.note_before = *existing;

    if (!score_->remove_note(id))
        return false;

    history_.record(step);
    return true;
}

bool ScoreEditor::modify_recorded(const notation::ScoreNote& before,
                                  const notation::ScoreNote& after)
{
    EditStep step;
    step.target = EditStep::Target::Note;
    step.staff_index = score_->staff_index_of(before.id);
    step.had_before = true;
    step.has_after = true;
    step.note_before = before;
    step.note_after = after;
    step.note_after.id = before.id;

    if (!score_->modify_note(before.id, step.note_after))
        return false;

    history_.record(step);
    return true;
}

std::size_t ScoreEditor::clear_span(notation::Ticks from, notation::Ticks to)
{
    return clear_span_on(cursor_.staff_index, from, to, cursor_.voice);
}

std::size_t ScoreEditor::clear_span_on(std::size_t staff_index, notation::Ticks from,
                                       notation::Ticks to, std::uint8_t voice)
{
    if (staff_index >= score_->staff_count() || to <= from)
        return 0;

    // Collected first: removing from the score invalidates the vector being
    // walked.
    std::vector<notation::EventId> doomed;
    for (const notation::ScoreNote& note : score_->notes(staff_index)) {
        if (note.voice != voice)
            continue;
        if (note.start_tick < to && note.end_tick() > from)
            doomed.push_back(note.id);
    }

    std::size_t removed = 0;
    for (const notation::EventId id : doomed) {
        if (remove_recorded(id))
            ++removed;
    }
    return removed;
}

std::uint8_t ScoreEditor::voice_landing(std::uint8_t source_voice, std::uint8_t lowest) const noexcept
{
    constexpr int kLastVoice = 3;
    const int offset = source_voice > lowest ? source_voice - lowest : 0;
    return static_cast<std::uint8_t>(std::min(cursor_.voice + offset, kLastVoice));
}

notation::Pitch ScoreEditor::spell_in_key(const notation::Pitch& pitch,
                                          notation::Ticks tick) const
{
    notation::Pitch spelled = pitch;
    if (cursor_.has_pending_accidental) {
        spelled.alteration = cursor_.pending_alteration;
        return spelled;
    }
    const notation::KeySignature key = score_->key_signature_at(tick);
    spelled.alteration = static_cast<std::int8_t>(key.alteration_for(spelled.step));
    return spelled;
}

// --- Entry ------------------------------------------------------------------

EditResult ScoreEditor::insert_pitch(const notation::Pitch& pitch)
{
    if (cursor_.staff_index >= score_->staff_count())
        return refused("The cursor is not on a staff");
    if (!cursor_.duration.is_valid())
        return refused("The selected duration is not one this model can write exactly");

    const notation::Ticks length = cursor_.duration.ticks();
    if (length <= 0)
        return refused("The selected duration has no length");

    notation::ScoreNote note;
    note.start_tick = cursor_.tick;
    note.duration = cursor_.duration;
    note.voice = cursor_.voice;
    note.tied_to_next = cursor_.tie_to_next;
    note.is_rest = cursor_.rest_mode;

    if (cursor_.grace_entry) {
        if (note.is_rest)
            return refused("A grace note cannot be a rest");
        // Written as an eighth unless something shorter is selected: a
        // grace note's value is how it looks, not how long it lasts, and
        // dots and tuplets have nothing to say about that.
        note.grace = true;
        note.tied_to_next = false;
        if (static_cast<int>(note.duration.value) < static_cast<int>(notation::NoteValue::Eighth))
            note.duration = notation::eighth();
        note.duration.dots = 0;
        note.duration.tuplet = notation::Tuplet{};
        // After the grace notes already before this tick in this voice.
        std::size_t before = 0;
        for (const notation::ScoreNote& existing : score_->notes(cursor_.staff_index)) {
            if (existing.grace && existing.start_tick == cursor_.tick
                && existing.voice == cursor_.voice)
                before = std::max<std::size_t>(before, existing.grace_order + 1u);
        }
        if (before > 255)
            return refused("No more grace notes fit before this note");
        note.grace_order = static_cast<std::uint8_t>(before);
    }

    if (!note.is_rest) {
        note.pitch = pitch;
        if (cursor_.has_pending_accidental)
            note.pitch.alteration = cursor_.pending_alteration;
        if (!note.pitch.is_valid())
            return refused("That is not a pitch this model can spell");
        if (notation::midi_note_number(note.pitch) < 0)
            return refused("That pitch is outside the MIDI range");
    }

    // Everything is checked before the first change, so the insert below
    // cannot fail half way and leave the score holding a removal that the
    // history does not know how to reverse.
    if (!note.is_valid())
        return refused("That note is not one this model can represent");

    history_.begin(note.grace ? "Insert grace note" : (note.is_rest ? "Insert rest" : "Insert note"),
                   cursor_);

    // A grace note takes no time, so it displaces nothing and the cursor
    // stays for the note it ornaments.
    const std::size_t overwritten =
        note.grace ? 0 : clear_span(cursor_.tick, cursor_.tick + length);
    const notation::EventId id = insert_recorded(cursor_.staff_index, note);
    if (id == notation::kInvalidEventId) {
        // Put back whatever the overwrite removed: the score must not be
        // left holding half an edit.
        history_.rollback(*score_);
        return refused("The note was rejected by the score");
    }

    last_entry_tick_ = cursor_.tick;
    last_grace_order_ = note.grace ? static_cast<int>(note.grace_order) : -1;
    if (!note.grace)
        cursor_.tick += length;
    cursor_.has_pending_accidental = false;
    cursor_.pending_alteration = 0;
    if (!note.grace)
        cursor_.tie_to_next = false;
    if (!note.is_rest)
        cursor_.entry_octave = note.pitch.octave;

    selection_.assign(1, id);
    history_.commit(cursor_);

    return applied(note.grace ? "Entered a grace note" : (note.is_rest ? "Entered a rest" : "Entered a note"),
                   overwritten + 1, id);
}

EditResult ScoreEditor::insert_step(notation::Step step)
{
    notation::Pitch pitch;
    pitch.step = step;
    pitch.octave = cursor_.entry_octave;
    return insert_pitch(spell_in_key(pitch, cursor_.tick));
}

EditResult ScoreEditor::insert_rest()
{
    const bool was_rest_mode = cursor_.rest_mode;
    cursor_.rest_mode = true;
    const EditResult result = insert_pitch(notation::Pitch{});
    cursor_.rest_mode = was_rest_mode;
    return result;
}

EditResult ScoreEditor::add_to_chord(const notation::Pitch& pitch)
{
    if (last_entry_tick_ < 0)
        return nothing("No note to add to - enter one first");
    if (cursor_.staff_index >= score_->staff_count())
        return refused("The cursor is not on a staff");

    notation::Pitch spelled = pitch;
    if (cursor_.has_pending_accidental)
        spelled.alteration = cursor_.pending_alteration;
    if (!spelled.is_valid() || notation::midi_note_number(spelled) < 0)
        return refused("That pitch is outside the MIDI range");

    // The chord takes its duration from what is already there, so adding a
    // pitch cannot silently retime the chord. After a grace note the chord
    // is that grace note's, not the note's at the same tick that it
    // ornaments.
    notation::Duration duration = cursor_.duration;
    std::vector<notation::ScoreNote> existing;
    for (const notation::ScoreNote& note : score_->notes(cursor_.staff_index)) {
        if (note.voice != cursor_.voice || note.start_tick != last_entry_tick_ || note.is_rest)
            continue;
        if (note.grace != (last_grace_order_ >= 0))
            continue;
        if (note.grace && static_cast<int>(note.grace_order) != last_grace_order_)
            continue;
        existing.push_back(note);
        duration = note.duration;
    }
    if (existing.empty())
        return nothing("Nothing at that position to add to");

    for (const notation::ScoreNote& note : existing) {
        if (note.pitch == spelled)
            return nothing("That pitch is already in the chord");
    }

    notation::ScoreNote note;
    note.start_tick = last_entry_tick_;
    note.duration = duration;
    note.pitch = spelled;
    note.voice = cursor_.voice;
    note.in_chord = true;
    // A grace chord: the new note is a grace note of the same order, drawn
    // on the same staff and slashed as the one it joins.
    note.grace = existing.front().grace;
    note.grace_order = existing.front().grace_order;
    note.grace_slashed = existing.front().grace_slashed;
    note.staff_shift = existing.front().staff_shift;
    if (!note.is_valid())
        return refused("That note is not one this model can represent");

    history_.begin("Add to chord", cursor_);

    // The notes already there become chord members too, so the whole chord
    // is marked rather than only the pitches added after the first.
    for (const notation::ScoreNote& member : existing) {
        if (member.in_chord)
            continue;
        notation::ScoreNote updated = member;
        updated.in_chord = true;
        modify_recorded(member, updated);
    }

    const notation::EventId id = insert_recorded(cursor_.staff_index, note);
    if (id == notation::kInvalidEventId) {
        history_.rollback(*score_);
        return refused("The note was rejected by the score");
    }

    cursor_.has_pending_accidental = false;
    cursor_.pending_alteration = 0;
    select(id);
    history_.commit(cursor_);

    return applied("Added a pitch to the chord", 1, id);
}

EditResult ScoreEditor::add_step_to_chord(notation::Step step)
{
    notation::Pitch pitch;
    pitch.step = step;
    pitch.octave = cursor_.entry_octave;
    // Spelled in the key where the chord is, which is the last entry's
    // tick rather than the cursor's: the cursor has moved on.
    return add_to_chord(spell_in_key(pitch, last_entry_tick_ < 0 ? cursor_.tick : last_entry_tick_));
}

EditResult ScoreEditor::toggle_grace_slash()
{
    const std::vector<notation::EventId> targets = target_notes();
    if (targets.empty())
        return nothing("Nothing selected to slash");

    std::size_t changed = 0;
    std::size_t slashed = 0;
    history_.begin("Grace slash", cursor_);
    for (const notation::EventId id : targets) {
        const notation::ScoreNote* existing = score_->find_note(id);
        if (existing == nullptr || !existing->grace)
            continue;
        notation::ScoreNote updated = *existing;
        updated.grace_slashed = !existing->grace_slashed;
        if (updated.grace_slashed)
            ++slashed;
        if (modify_recorded(*existing, updated))
            ++changed;
    }
    if (changed == 0) {
        history_.abandon();
        return nothing("No grace notes selected to slash");
    }
    history_.commit(cursor_);
    return applied(slashed > 0 ? "Slashed the grace notes" : "Took the slash off the grace notes",
                   changed);
}

EditResult ScoreEditor::insert_at(std::size_t staff_index, notation::Ticks tick,
                                  const notation::Pitch& pitch)
{
    if (staff_index >= score_->staff_count())
        return refused("There is no such staff");
    if (tick < 0)
        return refused("A note cannot start before the score does");

    cursor_.staff_index = staff_index;
    cursor_.tick = tick;
    return insert_pitch(pitch);
}

// --- Entry tools ------------------------------------------------------------

EditResult ScoreEditor::set_duration(notation::NoteValue value)
{
    notation::Duration wanted;
    wanted.value = value;

    if (!selection_.empty()) {
        // With a selection, choosing a duration retimes those notes, which
        // is what a selected note plus a duration key means. Dots and
        // tuplets are kept, since the user chose them for these notes.
        std::size_t changed = 0;
        history_.begin("Change duration", cursor_);
        for (const notation::EventId id : selection_) {
            const notation::ScoreNote* existing = score_->find_note(id);
            if (existing == nullptr)
                continue;

            notation::ScoreNote updated = *existing;
            updated.duration.value = value;
            if (!updated.duration.is_valid() || !updated.is_valid())
                continue;

            const notation::ScoreNote before = *existing;
            if (modify_recorded(before, updated))
                ++changed;
        }
        if (changed == 0) {
            history_.abandon();
            return nothing("Nothing in the selection could take that duration");
        }
        history_.commit(cursor_);
        return applied("Changed the duration of the selection", changed);
    }

    // Choosing a value clears the dots and tuplet, because the dot is
    // pressed after the value it modifies.
    cursor_.duration = wanted;
    return tools_applied("Selected a duration");
}

EditResult ScoreEditor::set_dots(int dots)
{
    if (dots < 0 || dots > 3)
        return refused("A note can carry at most three dots");

    notation::Duration wanted = cursor_.duration;
    wanted.dots = static_cast<std::uint8_t>(dots);
    if (!wanted.is_valid())
        return refused("That duration cannot be written exactly at this resolution");

    if (!selection_.empty()) {
        std::size_t changed = 0;
        history_.begin("Change dots", cursor_);
        for (const notation::EventId id : selection_) {
            const notation::ScoreNote* existing = score_->find_note(id);
            if (existing == nullptr)
                continue;

            notation::ScoreNote updated = *existing;
            updated.duration.dots = static_cast<std::uint8_t>(dots);
            if (!updated.duration.is_valid() || !updated.is_valid())
                continue;
            if (modify_recorded(*existing, updated))
                ++changed;
        }
        if (changed == 0) {
            history_.abandon();
            return nothing("Nothing in the selection could take those dots");
        }
        history_.commit(cursor_);
        return applied("Changed the dots on the selection", changed);
    }

    cursor_.duration = wanted;
    return tools_applied("Set the dots for entry");
}

EditResult ScoreEditor::toggle_dot()
{
    return set_dots(cursor_.duration.dots == 1 ? 0 : 1);
}

EditResult ScoreEditor::toggle_double_dot()
{
    return set_dots(cursor_.duration.dots == 2 ? 0 : 2);
}

EditResult ScoreEditor::toggle_tuplet(const notation::Tuplet& tuplet)
{
    if (!tuplet.is_valid())
        return refused("That is not a tuplet ratio this model represents");

    notation::Duration wanted = cursor_.duration;
    wanted.tuplet = cursor_.duration.tuplet == tuplet ? notation::Tuplet{} : tuplet;
    // Nothing to nest inside once the tuplet is off.
    if (!wanted.tuplet.is_tuplet())
        wanted.outer = notation::Tuplet{};
    if (!wanted.is_valid())
        return refused("That duration cannot be written exactly as a tuplet");

    cursor_.duration = wanted;
    return tools_applied(wanted.tuplet.is_tuplet() ? "Tuplet entry on" : "Tuplet entry off");
}

EditResult ScoreEditor::toggle_nested_tuplet()
{
    if (!cursor_.duration.tuplet.is_tuplet())
        return refused("Start a tuplet first: a nested tuplet goes inside one");

    notation::Duration wanted = cursor_.duration;
    wanted.outer = wanted.outer.is_tuplet() ? notation::Tuplet{} : wanted.tuplet;
    if (!wanted.is_valid())
        return refused("That duration cannot be written exactly inside a tuplet");

    cursor_.duration = wanted;
    return tools_applied(wanted.outer.is_tuplet() ? "Nested tuplet entry on"
                                                  : "Nested tuplet entry off");
}

EditResult ScoreEditor::toggle_rest_mode()
{
    cursor_.rest_mode = !cursor_.rest_mode;
    return tools_applied(cursor_.rest_mode ? "Entering rests" : "Entering notes");
}

EditResult ScoreEditor::toggle_grace_entry()
{
    cursor_.grace_entry = !cursor_.grace_entry;
    return tools_applied(cursor_.grace_entry ? "Entering grace notes" : "Entering notes");
}

EditResult ScoreEditor::set_accidental(int alteration)
{
    if (alteration < -2 || alteration > 2)
        return refused("This model writes at most a double accidental");

    if (!selection_.empty()) {
        std::size_t changed = 0;
        history_.begin("Change accidental", cursor_);
        for (const notation::EventId id : selection_) {
            const notation::ScoreNote* existing = score_->find_note(id);
            if (existing == nullptr || existing->is_rest)
                continue;

            notation::ScoreNote updated = *existing;
            updated.pitch.alteration = static_cast<std::int8_t>(alteration);
            if (!updated.pitch.is_valid() || notation::midi_note_number(updated.pitch) < 0)
                continue;
            if (modify_recorded(*existing, updated))
                ++changed;
        }
        if (changed == 0) {
            history_.abandon();
            return nothing("Nothing in the selection could take that accidental");
        }
        history_.commit(cursor_);
        return applied("Changed the accidental on the selection", changed);
    }

    cursor_.has_pending_accidental = true;
    cursor_.pending_alteration = static_cast<std::int8_t>(alteration);
    return tools_applied("The next note will carry that accidental");
}

EditResult ScoreEditor::cycle_enharmonic()
{
    const std::vector<notation::EventId> targets = target_notes();
    if (targets.empty())
        return nothing("Nothing selected to respell");

    std::size_t changed = 0;
    history_.begin("Respell", cursor_);
    for (const notation::EventId id : targets) {
        const notation::ScoreNote* existing = score_->find_note(id);
        if (existing == nullptr || existing->is_rest)
            continue;

        notation::Pitch respelled;
        if (!enharmonic_of(existing->pitch, respelled))
            continue;

        notation::ScoreNote updated = *existing;
        updated.pitch = respelled;
        if (modify_recorded(*existing, updated))
            ++changed;
    }
    if (changed == 0) {
        history_.abandon();
        return nothing("No other spelling of that note is available");
    }
    history_.commit(cursor_);
    return applied("Respelled", changed);
}

// --- Changing existing notes ------------------------------------------------

EditResult ScoreEditor::delete_selection()
{
    const std::vector<notation::EventId> targets = target_notes();
    if (targets.empty())
        return nothing("Nothing selected to delete");

    history_.begin("Delete", cursor_);
    std::size_t removed = 0;
    for (const notation::EventId id : targets) {
        if (remove_recorded(id))
            ++removed;
    }
    if (removed == 0) {
        history_.abandon();
        return nothing("Nothing was deleted");
    }

    selection_.clear();
    history_.commit(cursor_);
    return applied("Deleted", removed);
}

EditResult ScoreEditor::transpose_selection(int diatonic_steps)
{
    if (diatonic_steps == 0)
        return nothing("No interval to transpose by");

    const std::vector<notation::EventId> targets = target_notes();
    if (targets.empty())
        return nothing("Nothing selected to transpose");

    std::size_t changed = 0;
    history_.begin("Transpose", cursor_);
    for (const notation::EventId id : targets) {
        const notation::ScoreNote* existing = score_->find_note(id);
        if (existing == nullptr || existing->is_rest)
            continue;

        const int diatonic = notation::diatonic_index(existing->pitch) + diatonic_steps;
        if (diatonic < 0)
            continue;

        notation::Pitch moved;
        moved.octave = static_cast<std::int8_t>(diatonic / 7);
        moved.step = static_cast<notation::Step>(diatonic % 7);

        // Spelled by the key in force, so a step up from F sharp in D major
        // is G rather than G flat.
        const notation::KeySignature key = score_->key_signature_at(existing->start_tick);
        moved.alteration = static_cast<std::int8_t>(key.alteration_for(moved.step));

        if (!moved.is_valid() || notation::midi_note_number(moved) < 0)
            continue;

        notation::ScoreNote updated = *existing;
        updated.pitch = moved;
        if (modify_recorded(*existing, updated))
            ++changed;
    }
    if (changed == 0) {
        history_.abandon();
        return nothing("Nothing could move that far");
    }
    history_.commit(cursor_);
    return applied("Transposed", changed);
}

EditResult ScoreEditor::transpose_selection_octaves(int octaves)
{
    if (octaves == 0)
        return nothing("No interval to transpose by");

    const std::vector<notation::EventId> targets = target_notes();
    if (targets.empty())
        return nothing("Nothing selected to transpose");

    std::size_t changed = 0;
    history_.begin("Transpose octave", cursor_);
    for (const notation::EventId id : targets) {
        const notation::ScoreNote* existing = score_->find_note(id);
        if (existing == nullptr || existing->is_rest)
            continue;

        notation::ScoreNote updated = *existing;
        updated.pitch.octave = static_cast<std::int8_t>(updated.pitch.octave + octaves);
        if (!updated.pitch.is_valid() || notation::midi_note_number(updated.pitch) < 0)
            continue;
        if (modify_recorded(*existing, updated))
            ++changed;
    }
    if (changed == 0) {
        history_.abandon();
        return nothing("Nothing could move that far");
    }
    history_.commit(cursor_);
    return applied("Transposed by an octave", changed);
}

EditResult ScoreEditor::toggle_articulation(notation::Articulation articulation)
{
    const std::vector<notation::EventId> targets = target_notes();
    if (targets.empty())
        return nothing("Nothing selected to mark");

    const notation::ArticulationMask bit = notation::to_mask(articulation);
    if (bit == 0)
        return refused("That is not an articulation");

    std::size_t changed = 0;
    history_.begin("Articulation", cursor_);
    for (const notation::EventId id : targets) {
        const notation::ScoreNote* existing = score_->find_note(id);
        if (existing == nullptr)
            continue;

        notation::ScoreNote updated = *existing;
        updated.articulations ^= bit;
        if (modify_recorded(*existing, updated))
            ++changed;
    }
    if (changed == 0) {
        history_.abandon();
        return nothing("Nothing was marked");
    }
    history_.commit(cursor_);
    return applied("Toggled an articulation", changed);
}

EditResult ScoreEditor::cross_staff(int direction)
{
    if (direction != -1 && direction != 1)
        return refused("Cross-staff goes one staff up or down");

    const std::vector<notation::EventId> targets = target_notes();
    if (targets.empty())
        return nothing("Nothing selected to move");

    // Every member of each targeted chord: a chord split across the staves
    // is not drawn, so the chord moves whole.
    std::vector<notation::EventId> whole_chords;
    for (const notation::EventId id : targets) {
        const notation::ScoreNote* note = score_->find_note(id);
        if (note == nullptr || note->is_rest)
            continue;
        const std::size_t staff_index = score_->staff_index_of(id);
        if (staff_index >= score_->staff_count())
            continue;
        const int shown_on = static_cast<int>(staff_index) + direction;
        if (shown_on < 0 || shown_on >= static_cast<int>(score_->staff_count())) {
            // Unless the note is already drawn away and is coming back.
            if (note->staff_shift != direction)
                return refused(direction < 0 ? "There is no staff above to draw on"
                                             : "There is no staff below to draw on");
        }
        for (const notation::ScoreNote& member : score_->notes(staff_index)) {
            if (member.is_rest || member.start_tick != note->start_tick
                || member.voice != note->voice || member.grace != note->grace
                || member.grace_order != note->grace_order)
                continue;
            if (std::find(whole_chords.begin(), whole_chords.end(), member.id) == whole_chords.end())
                whole_chords.push_back(member.id);
        }
    }
    if (whole_chords.empty())
        return nothing("Nothing selected to move");

    std::size_t changed = 0;
    std::size_t moved_away = 0;
    history_.begin("Cross-staff", cursor_);
    for (const notation::EventId id : whole_chords) {
        const notation::ScoreNote* existing = score_->find_note(id);
        if (existing == nullptr)
            continue;
        notation::ScoreNote updated = *existing;
        updated.staff_shift = existing->staff_shift == direction ? 0 : static_cast<std::int8_t>(direction);
        if (updated.staff_shift != 0)
            ++moved_away;
        if (modify_recorded(*existing, updated))
            ++changed;
    }
    if (changed == 0) {
        history_.abandon();
        return nothing("Nothing was moved");
    }
    history_.commit(cursor_);
    if (moved_away == 0)
        return applied("Drawn on their own staff again", changed);
    return applied(direction < 0 ? "Drawn on the staff above" : "Drawn on the staff below", changed);
}

EditResult ScoreEditor::toggle_tie()
{
    const std::vector<notation::EventId> targets = target_notes();
    if (targets.empty()) {
        // Pressed before the note it applies to, which is how keyboard
        // entry works.
        cursor_.tie_to_next = !cursor_.tie_to_next;
        return tools_applied(cursor_.tie_to_next ? "The next note will be tied"
                                                 : "The next note will not be tied");
    }

    std::size_t changed = 0;
    history_.begin("Tie", cursor_);
    for (const notation::EventId id : targets) {
        const notation::ScoreNote* existing = score_->find_note(id);
        if (existing == nullptr || existing->is_rest)
            continue;

        notation::ScoreNote updated = *existing;
        updated.tied_to_next = !updated.tied_to_next;
        if (modify_recorded(*existing, updated))
            ++changed;
    }
    if (changed == 0) {
        history_.abandon();
        return nothing("A rest cannot be tied");
    }
    history_.commit(cursor_);
    return applied("Toggled a tie", changed);
}

// --- Markings ---------------------------------------------------------------

EditResult ScoreEditor::add_dynamic(notation::DynamicLevel level)
{
    notation::DynamicMarking marking;
    marking.tick = cursor_.tick;
    marking.level = level;
    marking.voice = cursor_.voice;
    marking.staff = static_cast<std::uint8_t>(std::min<std::size_t>(cursor_.staff_index, 255));
    if (!marking.is_valid())
        return refused("That dynamic is not one this model represents");

    EditStep step;
    step.target = EditStep::Target::Dynamic;
    step.has_after = true;
    step.dynamic_after = marking;

    for (const notation::DynamicMarking& existing : score_->dynamics()) {
        if (existing.tick == marking.tick && existing.voice == marking.voice
            && existing.staff == marking.staff) {
            if (existing.level == level && existing.text.empty())
                return nothing("That dynamic is already written there");
            step.had_before = true;
            step.dynamic_before = existing;
            break;
        }
    }

    history_.begin("Dynamic", cursor_);
    if (!apply_step(*score_, step, true)) {
        history_.abandon();
        return refused("The dynamic was rejected by the score");
    }
    history_.record(step);
    history_.commit(cursor_);
    return applied("Wrote a dynamic");
}

EditResult ScoreEditor::start_hairpin(notation::HairpinKind kind)
{
    const notation::Ticks length = cursor_.duration.ticks();
    if (length <= 0)
        return refused("The selected duration has no length");

    notation::Hairpin hairpin;
    hairpin.start_tick = cursor_.tick;
    hairpin.end_tick = cursor_.tick + length;
    hairpin.kind = kind;
    hairpin.voice = cursor_.voice;
    hairpin.staff = static_cast<std::uint8_t>(std::min<std::size_t>(cursor_.staff_index, 255));
    if (!hairpin.is_valid())
        return refused("That hairpin is not one this model represents");

    EditStep step;
    step.target = EditStep::Target::Hairpin;
    step.has_after = true;
    step.hairpin_after = hairpin;

    for (const notation::Hairpin& existing : score_->hairpins()) {
        if (existing.start_tick == hairpin.start_tick && existing.voice == hairpin.voice
            && existing.staff == hairpin.staff) {
            step.had_before = true;
            step.hairpin_before = existing;
            break;
        }
    }

    history_.begin("Hairpin", cursor_);
    if (!apply_step(*score_, step, true)) {
        history_.abandon();
        return refused("The hairpin was rejected by the score");
    }
    history_.record(step);
    history_.commit(cursor_);

    active_span_ = ActiveSpan{ActiveSpan::Kind::Hairpin, hairpin.start_tick, hairpin.end_tick,
                              hairpin.voice};
    return applied(kind == notation::HairpinKind::Crescendo ? "Started a crescendo"
                                                            : "Started a diminuendo");
}

EditResult ScoreEditor::start_slur()
{
    const notation::Ticks length = cursor_.duration.ticks();
    if (length <= 0)
        return refused("The selected duration has no length");

    notation::Slur slur;
    slur.start_tick = cursor_.tick;
    slur.end_tick = cursor_.tick + length;
    slur.voice = cursor_.voice;
    slur.staff = static_cast<std::uint8_t>(cursor_.staff_index);
    if (!slur.is_valid())
        return refused("That slur is not one this model represents");

    EditStep step;
    step.target = EditStep::Target::Slur;
    step.has_after = true;
    step.slur_after = slur;

    history_.begin("Slur", cursor_);
    if (!apply_step(*score_, step, true)) {
        history_.abandon();
        return refused("The slur was rejected by the score");
    }
    history_.record(step);
    history_.commit(cursor_);

    active_span_ =
        ActiveSpan{ActiveSpan::Kind::Slur, slur.start_tick, slur.end_tick, slur.voice};
    return applied("Started a slur");
}

bool ScoreEditor::has_active_span() const noexcept
{
    return active_span_.kind != ActiveSpan::Kind::None;
}

EditResult ScoreEditor::extend_span(int steps)
{
    if (active_span_.kind == ActiveSpan::Kind::None)
        return nothing("No hairpin or slur to stretch - start one first");
    if (steps == 0)
        return nothing("No amount to stretch by");

    const notation::Ticks length = cursor_.duration.ticks();
    if (length <= 0)
        return refused("The selected duration has no length");

    const notation::Ticks wanted_end =
        active_span_.end_tick + static_cast<notation::Ticks>(steps) * length;
    if (wanted_end <= active_span_.start_tick)
        return refused("A span cannot be shortened past its start");

    EditStep step;
    history_.begin("Stretch span", cursor_);

    if (active_span_.kind == ActiveSpan::Kind::Hairpin) {
        const notation::Hairpin* existing = nullptr;
        for (const notation::Hairpin& candidate : score_->hairpins()) {
            if (candidate.start_tick == active_span_.start_tick
                && candidate.voice == active_span_.voice)
                existing = &candidate;
        }
        if (existing == nullptr) {
            history_.abandon();
            active_span_ = ActiveSpan{};
            return nothing("That hairpin is no longer in the score");
        }

        step.target = EditStep::Target::Hairpin;
        step.had_before = true;
        step.hairpin_before = *existing;
        step.has_after = true;
        step.hairpin_after = *existing;
        step.hairpin_after.end_tick = wanted_end;
    } else {
        const notation::Slur* existing = nullptr;
        for (const notation::Slur& candidate : score_->slurs()) {
            if (candidate.start_tick == active_span_.start_tick
                && candidate.end_tick == active_span_.end_tick
                && candidate.voice == active_span_.voice)
                existing = &candidate;
        }
        if (existing == nullptr) {
            history_.abandon();
            active_span_ = ActiveSpan{};
            return nothing("That slur is no longer in the score");
        }

        step.target = EditStep::Target::Slur;
        step.had_before = true;
        step.slur_before = *existing;
        step.has_after = true;
        step.slur_after = *existing;
        step.slur_after.end_tick = wanted_end;
    }

    if (!apply_step(*score_, step, true)) {
        history_.abandon();
        return refused("The score refused the new span");
    }
    history_.record(step);
    history_.commit(cursor_);

    active_span_.end_tick = wanted_end;
    return applied(steps > 0 ? "Stretched the span" : "Shortened the span");
}

// --- Structure --------------------------------------------------------------

EditResult ScoreEditor::set_key_signature(notation::Ticks tick,
                                          const notation::KeySignature& key)
{
    if (!key.is_valid())
        return refused("That is not a key signature this model represents");
    if (tick < 0)
        return refused("A key signature cannot start before the score does");

    EditStep step;
    step.target = EditStep::Target::KeySignature;
    step.signature_tick = tick;
    step.has_after = true;
    step.key_after = key;

    for (const notation::KeySignatureChange& change : score_->key_signature_changes()) {
        if (change.tick == tick) {
            if (change.key == key)
                return nothing("That key signature is already in force there");
            step.had_before = true;
            step.key_before = change.key;
            break;
        }
    }

    history_.begin("Key signature", cursor_);
    if (!apply_step(*score_, step, true)) {
        history_.abandon();
        return refused("The score refused that key signature");
    }
    history_.record(step);
    history_.commit(cursor_);
    return applied("Changed the key signature");
}

EditResult ScoreEditor::set_time_signature(notation::Ticks tick,
                                           const music::TimeSignature& time_signature)
{
    if (!time_signature.is_valid())
        return refused("That is not a time signature this model represents");
    if (tick < 0)
        return refused("A time signature cannot start before the score does");

    EditStep step;
    step.target = EditStep::Target::TimeSignature;
    step.signature_tick = tick;
    step.has_after = true;
    step.meter_after = time_signature;

    for (const notation::TimeSignatureChange& change : score_->time_signature_changes()) {
        if (change.tick == tick) {
            if (change.time_signature == time_signature)
                return nothing("That meter is already in force there");
            step.had_before = true;
            step.meter_before = change.time_signature;
            break;
        }
    }

    history_.begin("Time signature", cursor_);
    if (!apply_step(*score_, step, true)) {
        history_.abandon();
        // The score refuses a meter change off a barline, which would put
        // the bar structure permanently out of step with the host.
        return refused("A meter change has to fall on a barline");
    }
    history_.record(step);
    history_.commit(cursor_);
    return applied("Changed the time signature");
}

EditResult ScoreEditor::set_clef(std::size_t staff_index, notation::Clef clef)
{
    if (staff_index >= score_->staff_count())
        return refused("There is no such staff");
    if (score_->staff(staff_index).clef == clef)
        return nothing("That staff already uses that clef");

    EditStep step;
    step.target = EditStep::Target::Clef;
    step.staff_index = staff_index;
    step.had_before = true;
    step.has_after = true;
    step.clef_before = score_->staff(staff_index).clef;
    step.clef_after = clef;

    history_.begin("Clef", cursor_);
    if (!apply_step(*score_, step, true)) {
        history_.abandon();
        return refused("The score refused that clef");
    }
    history_.record(step);
    history_.commit(cursor_);
    return applied("Changed the clef");
}

EditResult ScoreEditor::set_staves(const std::vector<notation::Staff>& staves)
{
    if (staves.empty())
        return refused("A part needs at least one staff");

    const std::size_t have = score_->staff_count();
    for (std::size_t index = staves.size(); index < have; ++index) {
        if (!score_->notes(index).empty())
            return refused("A staff with music on it cannot be taken away");
    }

    const auto reads_alike = [](const notation::Staff& a, const notation::Staff& b) {
        return a.clef == b.clef && a.transposition_semitones == b.transposition_semitones
            && a.octave_shift == b.octave_shift && a.name == b.name;
    };

    std::vector<EditStep> steps;
    for (std::size_t index = 0; index < staves.size() && index < have; ++index) {
        if (reads_alike(score_->staff(index), staves[index]))
            continue;
        EditStep step;
        step.target = EditStep::Target::Staff;
        step.staff_index = index;
        step.had_before = true;
        step.has_after = true;
        step.staff_before = score_->staff(index);
        step.staff_after = staves[index];
        steps.push_back(step);
    }
    for (std::size_t index = have; index < staves.size(); ++index) {
        EditStep step;
        step.target = EditStep::Target::Staff;
        step.staff_index = index;
        step.has_after = true;
        step.staff_after = staves[index];
        steps.push_back(step);
    }
    // From the bottom up, since a staff is only ever taken from the bottom.
    for (std::size_t index = have; index > staves.size(); --index) {
        EditStep step;
        step.target = EditStep::Target::Staff;
        step.staff_index = index - 1;
        step.had_before = true;
        step.staff_before = score_->staff(index - 1);
        steps.push_back(step);
    }

    if (steps.empty())
        return nothing("The part is already written that way");

    history_.begin("Staves", cursor_);
    for (const EditStep& step : steps) {
        if (!apply_step(*score_, step, true)) {
            history_.rollback(*score_);
            return refused("The score refused the staves");
        }
        history_.record(step);
    }
    // A cursor on a staff that has gone moves up onto the last one left.
    set_cursor(cursor_);
    history_.commit(cursor_);
    return applied("Changed the staves");
}

EditResult ScoreEditor::add_text(notation::Ticks tick, const std::string& text)
{
    notation::TextMarking marking;
    marking.tick = tick;
    marking.voice = cursor_.voice;
    marking.text = text;
    if (!marking.is_valid())
        return refused("A text marking needs some text");

    EditStep step;
    step.target = EditStep::Target::Text;
    step.has_after = true;
    step.text_after = marking;

    for (const notation::TextMarking& existing : score_->texts()) {
        if (existing.tick == tick && existing.voice == marking.voice) {
            step.had_before = true;
            step.text_before = existing;
            break;
        }
    }

    history_.begin("Text", cursor_);
    if (!apply_step(*score_, step, true)) {
        history_.abandon();
        return refused("The text was rejected by the score");
    }
    history_.record(step);
    history_.commit(cursor_);
    return applied("Wrote a text marking");
}

// --- Clipboard --------------------------------------------------------------

EditResult ScoreEditor::copy()
{
    const std::vector<notation::EventId> targets = target_notes();
    if (targets.empty())
        return nothing("Nothing selected to copy");

    std::vector<notation::ScoreNote> gathered;
    for (const notation::EventId id : targets) {
        const notation::ScoreNote* existing = score_->find_note(id);
        if (existing != nullptr)
            gathered.push_back(*existing);
    }
    if (gathered.empty())
        return nothing("Nothing selected to copy");

    std::sort(gathered.begin(), gathered.end(),
              [](const notation::ScoreNote& a, const notation::ScoreNote& b) {
                  return a.start_tick < b.start_tick;
              });

    const notation::Ticks origin = gathered.front().start_tick;
    notation::Ticks end = origin;
    for (notation::ScoreNote& note : gathered) {
        end = std::max(end, note.end_tick());
        note.start_tick -= origin;
        // Ids belong to the notes in the score, not to the copy: a pasted
        // note is a new event.
        note.id = notation::kInvalidEventId;
    }

    clipboard_.notes = std::move(gathered);
    clipboard_.span = end - origin;
    return tools_applied("Copied");
}

EditResult ScoreEditor::cut()
{
    const EditResult copied = copy();
    if (!copied.succeeded())
        return copied;

    const EditResult deleted = delete_selection();
    if (!deleted.succeeded())
        return deleted;

    return applied("Cut", deleted.notes_changed);
}

EditResult ScoreEditor::paste()
{
    if (clipboard_.empty())
        return nothing("Nothing to paste");
    if (cursor_.staff_index >= score_->staff_count())
        return refused("The cursor is not on a staff");

    history_.begin("Paste", cursor_);

    // The copied voices, counted from the cursor's: a one-voice selection
    // lands in the voice being written, a two-voice passage keeps its
    // lines. What those voices held in the span goes, as entry does.
    std::uint8_t lowest = 3;
    for (const notation::ScoreNote& copied : clipboard_.notes)
        lowest = std::min(lowest, copied.voice);
    bool landing_used[4] = {false, false, false, false};
    for (const notation::ScoreNote& copied : clipboard_.notes)
        landing_used[voice_landing(copied.voice, lowest)] = true;
    std::size_t overwritten = 0;
    for (std::uint8_t voice = 0; voice < 4; ++voice) {
        if (landing_used[voice]) {
            overwritten += clear_span_on(cursor_.staff_index, cursor_.tick,
                                         cursor_.tick + clipboard_.span, voice);
        }
    }

    std::vector<notation::EventId> pasted;
    for (const notation::ScoreNote& copied : clipboard_.notes) {
        notation::ScoreNote note = copied;
        note.start_tick += cursor_.tick;
        note.voice = voice_landing(copied.voice, lowest);
        if (!note.is_valid())
            continue;

        const notation::EventId id = insert_recorded(cursor_.staff_index, note);
        if (id != notation::kInvalidEventId)
            pasted.push_back(id);
    }

    if (pasted.empty()) {
        history_.rollback(*score_);
        return refused("None of the copied notes could be pasted there");
    }

    cursor_.tick += clipboard_.span;
    selection_ = pasted;
    history_.commit(cursor_);
    return applied("Pasted", pasted.size() + overwritten);
}

EditResult ScoreEditor::paste_score(const notation::Score& source)
{
    if (cursor_.staff_index >= score_->staff_count())
        return refused("The cursor is not on a staff");
    if (source.total_note_count() == 0)
        return nothing("There is no music to place");

    const notation::Ticks at = cursor_.tick;
    const notation::Ticks span = source.end_tick();
    const std::size_t staves = std::min(source.staff_count(),
                                        score_->staff_count() - cursor_.staff_index);

    history_.begin("Place music", cursor_);

    // The source's voices, counted from the cursor's as paste counts
    // them: its lowest voice goes into the voice being written, the next
    // into the one after, so an imported file whose lines were told apart
    // keeps them. What those voices held in the span goes, as entry does.
    std::uint8_t lowest = 3;
    for (std::size_t staff = 0; staff < source.staff_count(); ++staff) {
        for (const notation::ScoreNote& original : source.notes(staff))
            lowest = std::min(lowest, original.voice);
    }

    std::size_t overwritten = 0;
    std::vector<notation::EventId> placed;
    for (std::size_t staff = 0; staff < staves; ++staff) {
        const std::size_t target = cursor_.staff_index + staff;
        bool landing_used[4] = {false, false, false, false};
        for (const notation::ScoreNote& original : source.notes(staff))
            landing_used[voice_landing(original.voice, lowest)] = true;
        for (std::uint8_t voice = 0; voice < 4; ++voice) {
            if (landing_used[voice])
                overwritten += clear_span_on(target, at, at + span, voice);
        }

        for (const notation::ScoreNote& original : source.notes(staff)) {
            notation::ScoreNote note = original;
            note.id = notation::kInvalidEventId;
            note.start_tick += at;
            note.voice = voice_landing(original.voice, lowest);
            if (!note.is_valid())
                continue;
            const notation::EventId id = insert_recorded(target, note);
            if (id != notation::kInvalidEventId)
                placed.push_back(id);
        }
    }

    if (placed.empty()) {
        history_.rollback(*score_);
        return refused("None of that music could be placed here");
    }

    // The source's dynamics, in the voices the music went into. One that
    // lands where this part already has one replaces it, as writing a
    // dynamic there would.
    // Onto the staff the source's staff lands on, as the notes do: the
    // cursor's staff for the source's first, the next for its second.
    const auto target_staff = [this](std::uint8_t source_staff) {
        const std::size_t staff = std::min(cursor_.staff_index + source_staff,
                                           score_->staff_count() > 0 ? score_->staff_count() - 1 : 0);
        return static_cast<std::uint8_t>(std::min<std::size_t>(staff, 255));
    };
    for (const notation::DynamicMarking& original : source.dynamics()) {
        notation::DynamicMarking marking = original;
        marking.tick += at;
        marking.voice = voice_landing(original.voice, lowest);
        marking.staff = target_staff(original.staff);

        EditStep step;
        step.target = EditStep::Target::Dynamic;
        step.has_after = true;
        step.dynamic_after = marking;
        for (const notation::DynamicMarking& existing : score_->dynamics()) {
            if (existing.tick == marking.tick && existing.voice == marking.voice
                && existing.staff == marking.staff) {
                step.had_before = true;
                step.dynamic_before = existing;
                break;
            }
        }
        if (apply_step(*score_, step, true))
            history_.record(step);
    }

    // Its text markings too - an imported cue's markers - in the voice
    // the music went into, replacing one already there at the same tick
    // the way a dynamic does.
    for (const notation::TextMarking& original : source.texts()) {
        notation::TextMarking marking = original;
        marking.tick += at;
        marking.voice = voice_landing(original.voice, lowest);

        EditStep step;
        step.target = EditStep::Target::Text;
        step.has_after = true;
        step.text_after = marking;
        for (const notation::TextMarking& existing : score_->texts()) {
            if (existing.tick == marking.tick && existing.voice == marking.voice) {
                step.had_before = true;
                step.text_before = existing;
                break;
            }
        }
        if (apply_step(*score_, step, true))
            history_.record(step);
    }

    cursor_.tick = at + span;
    selection_ = placed;
    history_.commit(cursor_);
    return applied("Placed the music", placed.size() + overwritten);
}

// --- History ----------------------------------------------------------------

EditResult ScoreEditor::undo()
{
    if (!history_.can_undo())
        return nothing("Nothing to undo");

    const std::string name = history_.undo_name();
    if (!history_.undo(*score_, cursor_))
        return refused("The undo history no longer matches the score");

    // Ids the undo removed must not stay selected.
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                    [this](notation::EventId id) {
                                        return score_->find_note(id) == nullptr;
                                    }),
                     selection_.end());
    active_span_ = ActiveSpan{};
    return applied("Undid the last edit");
}

EditResult ScoreEditor::redo()
{
    if (!history_.can_redo())
        return nothing("Nothing to redo");

    if (!history_.redo(*score_, cursor_))
        return refused("The redo history no longer matches the score");

    selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                    [this](notation::EventId id) {
                                        return score_->find_note(id) == nullptr;
                                    }),
                     selection_.end());
    active_span_ = ActiveSpan{};
    return applied("Redid the edit");
}

// --- Commands ---------------------------------------------------------------

EditResult ScoreEditor::apply(composer::Command command)
{
    using composer::Command;

    switch (command) {
        case Command::None:
            return nothing("No command");

        // --- Duration -------------------------------------------------------
        case Command::SelectDurationBreve:
            return set_duration(notation::NoteValue::Breve);
        case Command::SelectDurationWhole:
            return set_duration(notation::NoteValue::Whole);
        case Command::SelectDurationHalf:
            return set_duration(notation::NoteValue::Half);
        case Command::SelectDurationQuarter:
            return set_duration(notation::NoteValue::Quarter);
        case Command::SelectDurationEighth:
            return set_duration(notation::NoteValue::Eighth);
        case Command::SelectDurationSixteenth:
            return set_duration(notation::NoteValue::Sixteenth);
        case Command::SelectDurationThirtySecond:
            return set_duration(notation::NoteValue::ThirtySecond);
        case Command::SelectDurationSixtyFourth:
            return set_duration(notation::NoteValue::SixtyFourth);

        // --- Rhythmic modifiers ---------------------------------------------
        case Command::ToggleDot:       return toggle_dot();
        case Command::ToggleDoubleDot: return toggle_double_dot();
        case Command::ToggleTuplet:    return toggle_tuplet(notation::Tuplet{3, 2});
        case Command::ToggleNestedTuplet: return toggle_nested_tuplet();
        case Command::ToggleTie:       return toggle_tie();
        case Command::ToggleRest:      return toggle_rest_mode();
        case Command::ToggleGraceEntry: return toggle_grace_entry();
        case Command::ToggleGraceSlash: return toggle_grace_slash();
        case Command::CrossStaffUp:    return cross_staff(-1);
        case Command::CrossStaffDown:  return cross_staff(1);

        // --- Accidentals ----------------------------------------------------
        case Command::AccidentalSharp:       return set_accidental(1);
        case Command::AccidentalFlat:        return set_accidental(-1);
        case Command::AccidentalNatural:     return set_accidental(0);
        case Command::AccidentalDoubleSharp: return set_accidental(2);
        case Command::AccidentalDoubleFlat:  return set_accidental(-2);
        case Command::CycleEnharmonicSpelling: return cycle_enharmonic();

        // --- Pitch and movement ---------------------------------------------
        case Command::PitchUpStep:   return transpose_selection(1);
        case Command::PitchDownStep: return transpose_selection(-1);

        case Command::PitchUpOctave:
        case Command::PitchDownOctave: {
            const int octaves = command == Command::PitchUpOctave ? 1 : -1;
            if (!target_notes().empty())
                return transpose_selection_octaves(octaves);

            // With nothing selected, this moves the octave the next note
            // will be entered in - which is what it is for during entry.
            const int wanted = cursor_.entry_octave + octaves;
            if (wanted < -1 || wanted > 9)
                return refused("That is outside the range of written octaves");
            cursor_.entry_octave = static_cast<std::int8_t>(wanted);
            return tools_applied("Changed the entry octave");
        }

        case Command::MoveCursorLeft:  return move_cursor(CursorStep::Duration, -1);
        case Command::MoveCursorRight: return move_cursor(CursorStep::Duration, 1);
        case Command::MoveCursorMeasureLeft:  return move_cursor(CursorStep::Measure, -1);
        case Command::MoveCursorMeasureRight: return move_cursor(CursorStep::Measure, 1);
        case Command::MoveCursorStaffUp:   return move_cursor(CursorStep::Staff, -1);
        case Command::MoveCursorStaffDown: return move_cursor(CursorStep::Staff, 1);

        // --- Voices ---------------------------------------------------------
        case Command::SelectVoice1: return select_voice(0);
        case Command::SelectVoice2: return select_voice(1);
        case Command::SelectVoice3: return select_voice(2);
        case Command::SelectVoice4: return select_voice(3);

        // --- Articulations --------------------------------------------------
        case Command::ArticulationAccent:
            return toggle_articulation(notation::Articulation::Accent);
        case Command::ArticulationStaccato:
            return toggle_articulation(notation::Articulation::Staccato);
        case Command::ArticulationTenuto:
            return toggle_articulation(notation::Articulation::Tenuto);
        case Command::ArticulationMarcato:
            return toggle_articulation(notation::Articulation::Marcato);
        case Command::ArticulationStaccatissimo:
            return toggle_articulation(notation::Articulation::Staccatissimo);

        // --- Dynamics -------------------------------------------------------
        case Command::DynamicPianississimo:
            return add_dynamic(notation::DynamicLevel::Pianississimo);
        case Command::DynamicPianissimo:
            return add_dynamic(notation::DynamicLevel::Pianissimo);
        case Command::DynamicPiano:
            return add_dynamic(notation::DynamicLevel::Piano);
        case Command::DynamicMezzoPiano:
            return add_dynamic(notation::DynamicLevel::MezzoPiano);
        case Command::DynamicMezzoForte:
            return add_dynamic(notation::DynamicLevel::MezzoForte);
        case Command::DynamicForte:
            return add_dynamic(notation::DynamicLevel::Forte);
        case Command::DynamicFortissimo:
            return add_dynamic(notation::DynamicLevel::Fortissimo);
        case Command::DynamicFortississimo:
            return add_dynamic(notation::DynamicLevel::Fortississimo);

        // --- Spans ----------------------------------------------------------
        case Command::StartCrescendo:
            return start_hairpin(notation::HairpinKind::Crescendo);
        case Command::StartDiminuendo:
            return start_hairpin(notation::HairpinKind::Diminuendo);
        case Command::StartSlur:      return start_slur();
        case Command::ExtendSpanRight: return extend_span(1);
        case Command::ShrinkSpanLeft:  return extend_span(-1);

        // --- Structure ------------------------------------------------------
        // These need a value the keystroke does not carry. The caller asks
        // for one and then calls the matching method.
        case Command::EditKeySignature:
            return needs_input("Choose a key signature");
        case Command::EditTimeSignature:
            return needs_input("Choose a time signature");
        case Command::EditClef:
            return needs_input("Choose a clef");
        case Command::AddText:
            return needs_input("Type the text to write");

        case Command::EditTempo:
            // Tempo comes from the host session: the notation view follows
            // the Pro Tools timeline (spec 4.3, 4.4), so editing tempo here
            // would create a second source of truth for it. The
            // application writes tempo to the host instead.
            return not_handled("Tempo belongs to the host session, not to the score");

        // --- Editing --------------------------------------------------------
        case Command::Undo: return undo();
        case Command::Redo: return redo();
        case Command::Copy: return copy();
        case Command::Cut:  return cut();
        case Command::Paste: return paste();
        case Command::DeleteSelection: return delete_selection();
        case Command::SelectAll: return select_all();

        // --- Workflow -------------------------------------------------------
        // Not the editor's business, and saying so is better than silently
        // doing nothing: the application routes these.
        case Command::ToggleKeyboardEntryMode:
            return not_handled("Entry mode is the application's to switch");
        case Command::PlayFromCursor:
        case Command::StopPlayback:
            return not_handled("Playback is the host's, through the bridge");
        case Command::ResyncWithHost:
            return not_handled("Resynchronization is the application's");
        case Command::ShowComposerWindow:
            return not_handled("Window management is the application's");
        case Command::ExportMidiFile:
        case Command::PrintPart:
            return not_handled("Exporting is the application's");
        case Command::ImportMidiFile:
        case Command::ImportMidiFileIntoPart:
            return not_handled("Importing is the application's");
        case Command::CycleImportGrid:
        case Command::ToggleImportTriplets:
        case Command::ToggleImportLengths:
        case Command::CycleImportMinimumLength:
        case Command::ToggleImportClefs:
        case Command::CycleImportKey:
        case Command::ToggleImportDynamics:
        case Command::ToggleImportMarkers:
        case Command::CycleImportSplit:
        case Command::ToggleImportVoices:
            return not_handled("Import settings are the application's");
        case Command::SendToProTools:
            return not_handled("Placing music in the session is the application's");
        case Command::SaveDocument:
        case Command::SaveDocumentAs:
        case Command::OpenDocument:
        case Command::OpenRecentDocument:
        case Command::CloseDocument:
        case Command::CloseAllDocuments:
        case Command::NewDocument:
        case Command::RenameDocument:
        case Command::DuplicateDocument:
        case Command::MoveDocumentUp:
        case Command::MoveDocumentDown:
            return not_handled("Files are the application's");

        case Command::ZoomNotationIn:
        case Command::ZoomNotationOut:
        case Command::ResetNotationZoom:
            return not_handled("How large the notation is drawn is the window's");

        case Command::ScrollToPlayhead:
        case Command::ToggleFollowPlayhead:
            return not_handled("Where the view is looking is the window's");

        case Command::ToggleClick:
        case Command::ClickTempoSlower:
        case Command::ClickTempoFaster:
        case Command::ClickFollowsThePart:
            return not_handled("Making a sound is the application's");

        case Command::CycleExpressionMap:
        case Command::CopyExpressionMap:
        case Command::CycleExpressionMapDynamics:
        case Command::SetExpressionMapDynamicsController:
        case Command::SetExpressionMapKeyswitch:
            return not_handled("How a part sounds is the application's");

        case Command::CycleShortcutProfile:
        case Command::CopyShortcutProfile:
        case Command::SetShortcutBinding:
            return not_handled("Which keys do what is the application's");

        case Command::ChooseInstrument:
        case Command::SetPartTransposition:
            return not_handled("What a part is written for is the application's");
    }

    return not_handled("Unknown command");
}

}  // namespace vcc::editing
