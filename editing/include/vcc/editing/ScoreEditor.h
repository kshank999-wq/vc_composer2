// Copyright (c) VC Composer. All rights reserved.
//
// Notation editing (spec 02 sections 5.2, 5.3).
//
// Spec 5.2 asks for "keyboard-driven notation entry (note durations,
// accidentals, ties, articulations, dynamics)" and "mouse/pointer entry".
// This is the layer that performs those edits, driven either by the
// `composer::Command` values the shortcut profiles already bind, or by
// direct calls from the mouse path.
//
// Three things shape it:
//
//   * It edits a score it does not own. The score is the document; the
//     editor is a tool held against it. So there is no second copy to keep
//     in step, and a caller can point two editors at one score or throw the
//     editor away without losing anything.
//   * Every edit is one transaction, so undo matches what the user did
//     rather than what the code did. Entering a note over an existing one
//     is three changes and one undo step.
//   * Nothing here draws, and nothing here knows about the view. The view
//     reports a click as a staff and a tick; what that means musically is
//     decided here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vcc/composer/ShortcutProfile.h"
#include "vcc/editing/EditCursor.h"
#include "vcc/editing/EditHistory.h"
#include "vcc/notation/Score.h"

namespace vcc::editing {

enum class EditStatus : std::uint8_t {
    /// The score changed.
    Applied = 0,

    /// The command was understood and there was nothing to do - no
    /// selection to act on, nothing at the cursor, a toggle already in the
    /// requested state.
    NothingToDo,

    /// Not this layer's command. Playback, window management and host
    /// resynchronization belong to the application, and saying so is better
    /// than silently swallowing them.
    NotHandled,

    /// Understood, but it needs a value the command alone does not carry -
    /// which key, which meter, which clef. The caller asks, then calls the
    /// matching method.
    NeedsInput,

    /// Refused, with a reason: a pitch outside MIDI range, a meter change
    /// off a barline, a duration the model cannot express exactly.
    Refused,
};

const char* to_string(EditStatus status) noexcept;

struct EditResult {
    EditStatus status = EditStatus::NothingToDo;

    /// Why, for the status line. Always a sentence, never empty.
    const char* message = "";

    /// Notes added, removed or changed. 0 for an edit that only touched
    /// markings or the cursor.
    std::size_t notes_changed = 0;

    /// The note this edit produced, when it produced one.
    notation::EventId new_event = notation::kInvalidEventId;

    /// True when the document changed, as distinct from the tools changing.
    /// Selecting a duration or moving the cursor succeeds without the score
    /// being touched, and a caller that re-renders on every success would
    /// otherwise do it on every arrow key.
    bool score_changed = false;

    bool succeeded() const noexcept { return status == EditStatus::Applied; }
};

/// What a cursor movement moves by.
enum class CursorStep : std::uint8_t {
    /// The entry duration, so the cursor walks the rhythm being entered.
    Duration = 0,
    Measure,
    Staff,
};

/// Notes held for paste, with ticks relative to the earliest of them.
struct Clipboard {
    std::vector<notation::ScoreNote> notes;
    notation::Ticks span = 0;

    bool empty() const noexcept { return notes.empty(); }
    void clear();
};

class ScoreEditor {
public:
    /// The score must outlive the editor.
    explicit ScoreEditor(notation::Score& score);

    const notation::Score& score() const noexcept { return *score_; }

    // --- Cursor and selection ----------------------------------------------

    const EditCursor& cursor() const noexcept { return cursor_; }

    /// Moves the cursor. Clamped to the score's staves and to tick 0.
    void set_cursor(const EditCursor& cursor);

    EditResult move_cursor(CursorStep step, int direction);

    const std::vector<notation::EventId>& selection() const noexcept { return selection_; }
    void set_selection(std::vector<notation::EventId> selection);
    void select(notation::EventId id);
    void clear_selection();
    EditResult select_all();

    /// Notes at the cursor: the ones a command with no selection acts on.
    std::vector<notation::EventId> notes_at_cursor() const;

    /// Selects one note alone and takes the cursor to its staff and voice,
    /// which is what a click on a note does. The cursor's tick stays where
    /// it was, so typing after a click does not overwrite the note clicked.
    EditResult select_and_follow(notation::EventId id);

    // --- Voices -------------------------------------------------------------

    /// How many voices a staff holds. Four is the convention every notation
    /// program shares, and more than a keyboard part needs.
    static constexpr std::uint8_t kVoiceCount = 4;

    /// Chooses the voice entry writes into, 0-based.
    ///
    /// With a selection, moves those notes into that voice instead - one
    /// undo step, and the notes keep their ids, so the host sees them
    /// changed rather than deleted and re-added. A note moved onto a tick
    /// the voice already holds a note at joins it as a chord, which is what
    /// the score's ordering makes of two notes sharing a tick and voice.
    EditResult select_voice(std::uint8_t voice);

    // --- Entry --------------------------------------------------------------

    /// Enters a note at the cursor, using the cursor's duration, and
    /// advances past it.
    ///
    /// Anything already occupying that span in the same voice is removed -
    /// which is what entry modes do, and what the alternative rules out:
    /// splitting the existing note would write a tie chain the composer did
    /// not ask for. The removal and the insertion are one undo step.
    ///
    /// A pending accidental is applied and cleared. With no pending
    /// accidental the pitch is spelled by the key signature in force.
    EditResult insert_pitch(const notation::Pitch& pitch);

    /// Enters a note by step, taking the octave from the cursor. This is
    /// what a letter key does.
    EditResult insert_step(notation::Step step);

    EditResult insert_rest();

    /// Adds a pitch to the note just entered, without advancing. After a
    /// grace note, adds to that grace note, making a grace chord.
    EditResult add_to_chord(const notation::Pitch& pitch);

    /// The same by step, taking the octave from the cursor and the
    /// spelling from the key in force, as a letter key does. This is what
    /// Shift and a letter do in note entry.
    EditResult add_step_to_chord(notation::Step step);

    /// Puts the acciaccatura's slash on the targeted grace notes, or takes
    /// it off: each is toggled. Notes that are not grace notes are passed
    /// over; nothing to toggle is said.
    EditResult toggle_grace_slash();

    /// Mouse entry (spec 5.2): a staff, a tick and a pitch, as the view's
    /// hit testing reports them. Moves the cursor there first, so entry
    /// continues from where the click landed.
    EditResult insert_at(std::size_t staff_index, notation::Ticks tick,
                         const notation::Pitch& pitch);

    // --- Entry tools --------------------------------------------------------

    EditResult set_duration(notation::NoteValue value);
    EditResult set_dots(int dots);
    EditResult toggle_dot();
    EditResult toggle_double_dot();
    EditResult toggle_tuplet(const notation::Tuplet& tuplet);

    /// Nests the tuplet being written inside one of its own ratio - a
    /// triplet inside a triplet - or takes the nesting off again. Refused
    /// while no tuplet is being written, since there is nothing to nest
    /// inside.
    EditResult toggle_nested_tuplet();
    EditResult toggle_rest_mode();

    /// Grace-note entry on or off. While on, each pitch entered becomes a
    /// grace note before whatever is (or will be) at the cursor's tick in
    /// its voice, after the grace notes already there, written as an
    /// eighth (or the shorter value selected); the cursor does not move,
    /// so the note they ornament is typed next, as it is read.
    EditResult toggle_grace_entry();

    /// Draws the targeted notes on the staff above (`direction` -1) or below
    /// (+1) the one they belong to - the whole of each chord they are in,
    /// since a chord split across the staves is not drawn - or, when they
    /// are already drawn there, puts them back. Refused when the score has
    /// no staff in that direction.
    EditResult cross_staff(int direction);

    /// Sets the accidental for the next note entered, or respells the
    /// selection when there is one - which is the behaviour of pressing an
    /// accidental with a note selected.
    EditResult set_accidental(int alteration);

    /// Respells the selection to the other spelling of the same sound:
    /// F sharp becomes G flat.
    EditResult cycle_enharmonic();

    // --- Changing existing notes -------------------------------------------

    EditResult delete_selection();

    /// Moves the selection by diatonic steps, spelling the result with the
    /// key signature in force - so a step up from F sharp in D major is G,
    /// not G flat.
    EditResult transpose_selection(int diatonic_steps);
    EditResult transpose_selection_octaves(int octaves);

    EditResult toggle_articulation(notation::Articulation articulation);

    /// Ties the selection into what follows. With no selection this sets
    /// the entry flag, so a tie can be pressed before the note it applies
    /// to, as keyboard entry expects.
    EditResult toggle_tie();

    // --- Markings -----------------------------------------------------------

    EditResult add_dynamic(notation::DynamicLevel level);
    EditResult start_hairpin(notation::HairpinKind kind);
    EditResult start_slur();

    /// Lengthens or shortens the span started most recently, by the entry
    /// duration. This is how spec 5.3's "hairpins that can be stretched
    /// across beats/measures" is driven from the keyboard.
    EditResult extend_span(int steps);

    /// True while a hairpin or slur just started can still be stretched,
    /// so a control for stretching can say whether there is anything to
    /// stretch.
    bool has_active_span() const noexcept;

    // --- Structure ----------------------------------------------------------

    EditResult set_key_signature(notation::Ticks tick, const notation::KeySignature& key);
    EditResult set_time_signature(notation::Ticks tick,
                                  const music::TimeSignature& time_signature);
    EditResult set_clef(std::size_t staff_index, notation::Clef clef);

    /// Makes the score's staves read as `staves` do: each existing staff
    /// takes the clef, transposition, octave shift and name given for it,
    /// staves beyond the score's are added at the bottom, and staves
    /// beyond the list's are taken away from the bottom. One undo step,
    /// which is what lets choosing an instrument be undone like any edit.
    ///
    /// Refuses an empty list, and refuses to take away a staff with notes
    /// on it - the whole part would go with them. The music on the staves
    /// that stay is untouched: notes are written pitches, and what they
    /// sound as is the staff's business.
    EditResult set_staves(const std::vector<notation::Staff>& staves);
    EditResult add_text(notation::Ticks tick, const std::string& text);

    // --- Clipboard ----------------------------------------------------------

    EditResult copy();
    EditResult cut();

    /// Puts the copied notes at the cursor. Their voices are counted from
    /// the cursor's: the lowest voice among them goes into the voice being
    /// written, the next into the one after, and so on, so a one-voice
    /// selection lands in the cursor's voice and a two-voice passage
    /// keeps its lines. Whatever those voices held in the span is
    /// replaced, as entry does.
    EditResult paste();
    const Clipboard& clipboard() const noexcept { return clipboard_; }

    /// Puts another score's music into this one at the cursor, as one undo
    /// step: the way a MIDI file is imported *into* a part rather than as
    /// one (docs/aax/17-midi-import.md).
    ///
    /// The source's first staff goes onto the cursor's staff, its second
    /// onto the next, and so on; staves the part does not have are left
    /// out, and `notes_changed` counts what was placed. The voices are
    /// counted from the cursor's as `paste` counts them - the source's
    /// lowest voice goes into the voice being written, the next into the
    /// one after - and whatever those voices held in the span is replaced,
    /// which is what entry does. The source's dynamics come too, in those
    /// voices. Its tempo, meter, key and clefs do not: those are the
    /// part's, and the host's, not the file's.
    ///
    /// The cursor ends after what was placed, and the placed notes are the
    /// selection, so the next command acts on them.
    EditResult paste_score(const notation::Score& source);

    // --- History ------------------------------------------------------------

    EditResult undo();
    EditResult redo();

    EditHistory& history() noexcept { return history_; }
    const EditHistory& history() const noexcept { return history_; }

    // --- Commands -----------------------------------------------------------

    /// Performs a bound command (spec 8, 5.2). Every command the shortcut
    /// profiles carry is either performed here or reported as belonging
    /// elsewhere - none is silently ignored.
    EditResult apply(composer::Command command);

private:
    /// A span the keyboard can still stretch.
    struct ActiveSpan {
        enum class Kind : std::uint8_t { None = 0, Hairpin, Slur };
        Kind kind = Kind::None;
        notation::Ticks start_tick = 0;
        notation::Ticks end_tick = 0;
        std::uint8_t voice = 0;
    };

    /// Removes everything in `[from, to)` on the cursor's staff and voice,
    /// recording each removal. Returns how many notes went.
    std::size_t clear_span(notation::Ticks from, notation::Ticks to);

    /// The same, on any staff and voice.
    std::size_t clear_span_on(std::size_t staff_index, notation::Ticks from,
                              notation::Ticks to, std::uint8_t voice);

    /// The voice a source's voice lands in when its music is placed at
    /// the cursor: the cursor's for the source's lowest, the next for the
    /// next, and the fourth for anything beyond it.
    std::uint8_t voice_landing(std::uint8_t source_voice, std::uint8_t lowest) const noexcept;

    /// Inserts a note and records it. Returns its id, or kInvalidEventId.
    notation::EventId insert_recorded(std::size_t staff_index, const notation::ScoreNote& note);

    /// Replaces a note and records it.
    bool modify_recorded(const notation::ScoreNote& before, const notation::ScoreNote& after);

    /// Removes a note and records it.
    bool remove_recorded(notation::EventId id);

    /// The notes a command acts on: the selection, or what is at the cursor.
    std::vector<notation::EventId> target_notes() const;

    notation::Pitch spell_in_key(const notation::Pitch& pitch, notation::Ticks tick) const;

    notation::Score* score_ = nullptr;
    EditCursor cursor_{};
    std::vector<notation::EventId> selection_;
    EditHistory history_;
    Clipboard clipboard_;
    ActiveSpan active_span_{};

    /// Tick of the note entered most recently, so a chord can be built on
    /// it after the cursor has advanced - and, when that note was a grace
    /// note, its order, so the chord is built on the grace note rather
    /// than on the note it ornaments at the same tick. -1 for a plain note.
    notation::Ticks last_entry_tick_ = -1;
    int last_grace_order_ = -1;
};

}  // namespace vcc::editing
