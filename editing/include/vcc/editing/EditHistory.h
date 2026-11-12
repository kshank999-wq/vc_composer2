// Copyright (c) VC Composer. All rights reserved.
//
// Undo and redo (spec 02 section 5.2).
//
// Recorded as a journal of primitive changes with their inverses, not as
// snapshots of the score. Snapshots would be simpler, and wrong for the
// reason spec 13 gives: every note carries a stable id that links it to the
// MIDI events in Pro Tools, and undoing has to restore *those* events, not
// equivalent new ones. A journal can put a note back with the id it had; a
// snapshot restore would be indistinguishable from one, but only because
// snapshots of a large score cost more memory than a session can spare -
// a few hundred undo levels of a full cue is tens of megabytes.
//
// So each step records what a thing was and what it became. Undo applies
// them backwards, redo forwards, and both go through the same code, which
// is the only way to be sure they agree.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "vcc/editing/EditCursor.h"
#include "vcc/notation/Score.h"

namespace vcc::editing {

/// One primitive change, with everything needed to reverse it.
///
/// A single struct with a field per target rather than a variant: the
/// alternative is a tagged union whose every use site needs a visitor, for
/// a type that is written in one place and read in one place. It costs a
/// few hundred bytes per step and is far easier to read.
struct EditStep {
    enum class Target : std::uint8_t {
        Note = 0,
        Dynamic,
        Hairpin,
        Slur,
        TimeSignature,
        KeySignature,
        Clef,
        Text,

        /// A whole staff: its clef, transposition, octave shift and name
        /// together, or the staff itself. Both `before` and `after` set is
        /// a change to how the staff reads; only `after` is a staff added
        /// at the bottom; only `before` is the bottom staff taken away,
        /// which the score allows only while it is empty - so the step is
        /// reversible either way.
        Staff,
    };

    Target target = Target::Note;
    std::size_t staff_index = 0;

    /// What the thing was, and what it became. Neither set is meaningless;
    /// only `before` is a deletion, only `after` is an insertion, and both
    /// is a modification.
    bool had_before = false;
    bool has_after = false;

    notation::ScoreNote note_before{};
    notation::ScoreNote note_after{};

    notation::DynamicMarking dynamic_before{};
    notation::DynamicMarking dynamic_after{};

    notation::Hairpin hairpin_before{};
    notation::Hairpin hairpin_after{};

    notation::Slur slur_before{};
    notation::Slur slur_after{};

    notation::Ticks signature_tick = 0;
    music::TimeSignature meter_before{};
    music::TimeSignature meter_after{};
    notation::KeySignature key_before{};
    notation::KeySignature key_after{};

    notation::Clef clef_before = notation::Clef::Treble;
    notation::Clef clef_after = notation::Clef::Treble;

    notation::TextMarking text_before{};
    notation::TextMarking text_after{};

    notation::Staff staff_before{};
    notation::Staff staff_after{};
};

/// Applies one step to a score, forwards or backwards.
///
/// Returns false when the score is not in the state the step expects, which
/// means the history and the score have diverged - a bug, not a user error,
/// and better reported than silently half-applied.
bool apply_step(notation::Score& score, const EditStep& step, bool forward);

/// One user action: everything that has to be undone together.
///
/// Overwriting an existing note while entering a new one is three primitive
/// changes and one undo step, which is what a user means by "undo that".
struct Transaction {
    std::string name;  ///< For an Undo menu: "Insert note", "Delete".
    std::vector<EditStep> steps;

    EditCursor cursor_before{};
    EditCursor cursor_after{};

    bool empty() const noexcept { return steps.empty(); }
};

/// The undo and redo stacks.
class EditHistory {
public:
    /// `depth` transactions are kept; older ones fall off the bottom.
    explicit EditHistory(std::size_t depth = 200);

    /// Starts recording. A transaction already open is abandoned first,
    /// which is the right response to an editor that failed part way
    /// through: the steps it recorded were already applied, so keeping them
    /// would make undo replay a partial edit.
    void begin(const std::string& name, const EditCursor& cursor_before);

    /// Records a change that has already been applied to the score.
    void record(const EditStep& step);

    /// Ends recording and pushes the transaction. Returns false when
    /// nothing was recorded, in which case nothing is pushed - an edit that
    /// changed nothing must not consume an undo step.
    bool commit(const EditCursor& cursor_after);

    /// Discards an open transaction without pushing it.
    ///
    /// Only safe when nothing was applied to the score yet. When steps have
    /// already been applied, use `rollback`.
    void abandon();

    /// Reverses everything recorded in the open transaction and then
    /// discards it.
    ///
    /// This is what an edit that fails part way through needs. An edit that
    /// removes three notes and then finds it cannot insert the fourth has
    /// already changed the score; abandoning there would leave those
    /// removals in place with nothing able to undo them.
    ///
    /// Returns false when a step could not be reversed, which means the
    /// score and the history have diverged.
    bool rollback(notation::Score& score);

    bool is_recording() const noexcept { return recording_; }

    bool can_undo() const noexcept { return !undo_.empty(); }
    bool can_redo() const noexcept { return !redo_.empty(); }

    /// Name of the next undo or redo, for a menu. Empty when there is none.
    const std::string& undo_name() const noexcept;
    const std::string& redo_name() const noexcept;

    /// Applies the next undo or redo, and moves the cursor with it.
    bool undo(notation::Score& score, EditCursor& cursor);
    bool redo(notation::Score& score, EditCursor& cursor);

    void clear();

    std::size_t undo_depth() const noexcept { return undo_.size(); }
    std::size_t redo_depth() const noexcept { return redo_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }

private:
    void push(Transaction transaction);

    std::vector<Transaction> undo_;
    std::vector<Transaction> redo_;

    Transaction open_{};
    bool recording_ = false;
    std::size_t capacity_ = 200;
};

}  // namespace vcc::editing
