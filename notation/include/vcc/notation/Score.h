// Copyright (c) VC Composer. All rights reserved.
//
// The score model (spec 02 sections 5.1, 5.2, 12, 13).
//
// Host-independent by construction, as spec 13 requires: "Separate the
// notation engine from the host adapter so the score model can be tested
// independently of Pro Tools." Nothing here knows about AAX, JUCE, MIDI or
// Pro Tools. It is the NotationEvent entity of the spec 12 data model,
// plus the structure needed to place events musically.
//
// Two decisions shape everything else:
//
//   * Every note carries a stable id, because spec 13 requires "stable IDs
//     linking notation events to host MIDI events" and an event-diff layer
//     rather than rewriting the whole region on each edit. Ids survive edits
//     to other notes, so a diff can say "this note changed" instead of
//     "everything changed".
//   * Time is integer ticks throughout, so musical positions are exact.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vcc/music/MusicalTime.h"
#include "vcc/notation/Markings.h"
#include "vcc/notation/MusicalTime.h"
#include "vcc/notation/Pitch.h"

namespace vcc::notation {

/// Stable identifier for a score event. Never reused within a score.
using EventId = std::uint64_t;

constexpr EventId kInvalidEventId = 0;

/// A notated note or rest.
///
/// One type for both, distinguished by `is_rest`, because they occupy time
/// in a voice identically and almost every operation treats them the same.
struct ScoreNote {
    EventId id = kInvalidEventId;

    Ticks start_tick = 0;
    Duration duration{};

    /// Meaningless when `is_rest` is true.
    Pitch pitch{};
    bool is_rest = false;

    /// Voice within the staff, 0-based. Notes of different voices share a
    /// staff without displacing each other: entry, chords, ties and the
    /// engraver's stems all work within a voice.
    std::uint8_t voice = 0;

    /// Tied into the following note of the same pitch and voice. A tie
    /// chain sounds as one MIDI note, which the renderer relies on.
    bool tied_to_next = false;

    ArticulationMask articulations = 0;

    /// Chord member. Notes sharing a start tick and voice are one chord and
    /// are rendered as simultaneous MIDI notes.
    bool in_chord = false;

    /// A grace note: an ornament before the note at `start_tick` in this
    /// voice, taking no time of its own - its `end_tick()` is its start,
    /// so it neither fills the bar nor displaces what follows. The
    /// duration is how it is written (an eighth, usually), not how long it
    /// sounds; the MIDI renderer plays it just before the beat. Never a
    /// rest and never tied.
    bool grace = false;

    /// Its place among the grace notes before the same tick in the same
    /// voice, 0 first. Grace notes sharing an order are a grace chord.
    std::uint8_t grace_order = 0;

    /// The acciaccatura's slash through the stem. Off for an appoggiatura,
    /// written the same but unslashed.
    bool grace_slashed = true;

    /// Drawn on the staff above (-1) or below (+1) the one it belongs to,
    /// as a piano part crosses its hands: the note keeps its staff, voice,
    /// instrument and MIDI, and only the page moves it. A beam group whose
    /// notes are drawn on both staves runs between them. Meaningless on a
    /// score with no staff in that direction, where the note is drawn
    /// where it belongs.
    std::int8_t staff_shift = 0;

    Ticks end_tick() const noexcept { return grace ? start_tick : start_tick + duration.ticks(); }
    bool is_valid() const noexcept;
};

/// One staff of the score (spec 5.1).
struct Staff {
    Clef clef = Clef::Treble;

    /// Written-to-sounding transposition in semitones. A B-flat clarinet
    /// part is written a major second above what it sounds, so its staff
    /// carries -2. Applied when rendering to MIDI, never when displaying
    /// (spec 5.1: "instrument-aware transposition/display rules").
    std::int8_t transposition_semitones = 0;

    /// Additional octave displacement from an octave clef, in octaves.
    std::int8_t octave_shift = 0;

    std::string name;  ///< "Violin I", "Piano (L.H.)".

    /// Total sounding shift in semitones.
    int total_transposition() const noexcept
    {
        return transposition_semitones + octave_shift * 12;
    }
};

/// A time-signature change, at a measure boundary.
struct TimeSignatureChange {
    Ticks tick = 0;
    music::TimeSignature time_signature{4, 4};

    bool is_valid() const noexcept { return tick >= 0 && time_signature.is_valid(); }
};

/// A key-signature change.
struct KeySignatureChange {
    Ticks tick = 0;
    KeySignature key{};

    bool is_valid() const noexcept { return tick >= 0 && key.is_valid(); }
};

/// A measure, derived rather than stored: measure boundaries follow from the
/// time-signature changes, so they cannot drift out of agreement with them.
struct Measure {
    std::int32_t number = 1;      ///< 1-based, as a musician counts.
    Ticks start_tick = 0;
    Ticks length_ticks = 0;
    music::TimeSignature time_signature{4, 4};
    KeySignature key{};

    Ticks end_tick() const noexcept { return start_tick + length_ticks; }
    bool contains(Ticks tick) const noexcept
    {
        return tick >= start_tick && tick < end_tick();
    }
};

/// Ticks in one measure of a time signature.
Ticks measure_length_ticks(const music::TimeSignature& time_signature) noexcept;

/// A position expressed the way a musician reads it.
struct BarBeat {
    std::int32_t measure = 1;   ///< 1-based.
    double beat = 1.0;          ///< 1-based, fractional within the beat.
};

/// The score for one bound Pro Tools track (spec 12: TrackBinding carries a
/// staff config; this is the notation content for it).
///
/// Grand staff is two staves in one Score, as spec 5.1 requires for piano.
class Score {
public:
    Score();

    // --- Structure ----------------------------------------------------------

    /// Staves, in top-to-bottom score order. Always at least one.
    const std::vector<Staff>& staves() const noexcept { return staves_; }
    std::size_t staff_count() const noexcept { return staves_.size(); }

    Staff& staff(std::size_t index);
    const Staff& staff(std::size_t index) const;

    /// Replaces how a staff reads - clef, transposition, name - keeping
    /// its music, and counts as a change to the score. Writing through
    /// `staff()` does not bump the revision, which is how a clef change
    /// once left a part looking saved. False for an index that is not
    /// there.
    bool set_staff(std::size_t index, const Staff& staff);

    /// Adds a staff and returns its index. A second staff makes this a grand
    /// staff (spec 5.1).
    std::size_t add_staff(const Staff& staff);

    /// Configures as a grand staff for piano and similar: treble above bass.
    void make_grand_staff();

    /// Replaces the staves, discarding whatever music was on them.
    ///
    /// The one thing `add_staff` cannot express. A score that can gain a
    /// staff but never lose one cannot represent a part being
    /// reconfigured - and cannot be loaded into from a file with fewer
    /// staves than it already has, which is how a reopened part would grow
    /// a staff every time.
    ///
    /// Refuses an empty list: a score always has at least one staff, and
    /// that is what lets every lookup return a staff rather than an
    /// optional. Event ids are deliberately not reset, for the reason
    /// `clear` does not reset them either.
    bool reset_staves(const std::vector<Staff>& staves);

    /// Removes one staff, keeping the music on the others.
    ///
    /// Refuses the only staff, and refuses a staff with notes on it: a
    /// composer who wants a piano part to be a cello part has to take the
    /// left hand off first, since a staff that disappeared with its music
    /// is the one edit nothing could undo. Slurs on the staff go with it,
    /// and slurs on the staves below move up a number.
    bool remove_staff(std::size_t index);

    // --- Signatures ---------------------------------------------------------

    /// Sets the signature in force from `tick` onward. A change at a tick
    /// that already has one replaces it.
    ///
    /// Returns false for an invalid signature, or for a time-signature change
    /// that does not fall on a measure boundary - a mid-measure meter change
    /// is not something this model represents, and accepting one would put
    /// the bar structure permanently out of step with the host (spec 4.3).
    bool set_time_signature(Ticks tick, const music::TimeSignature& time_signature);
    bool set_key_signature(Ticks tick, const KeySignature& key);

    music::TimeSignature time_signature_at(Ticks tick) const noexcept;
    KeySignature key_signature_at(Ticks tick) const noexcept;

    const std::vector<TimeSignatureChange>& time_signature_changes() const noexcept
    {
        return time_signatures_;
    }
    const std::vector<KeySignatureChange>& key_signature_changes() const noexcept
    {
        return key_signatures_;
    }

    // --- Measures -----------------------------------------------------------

    /// The measure containing `tick`. Measures are computed from the
    /// time-signature changes on demand.
    Measure measure_at(Ticks tick) const noexcept;

    /// The measure with this 1-based number.
    Measure measure(std::int32_t number) const noexcept;

    /// Start tick of a 1-based measure number.
    Ticks measure_start_tick(std::int32_t number) const noexcept;

    /// Converts to and from bar/beat, which is how the UI and the host
    /// timeline both speak (spec 4.3, acceptance criterion 3).
    BarBeat to_bar_beat(Ticks tick) const noexcept;
    Ticks from_bar_beat(const BarBeat& position) const noexcept;

    // --- Notes --------------------------------------------------------------

    /// Notes on a staff, ordered by start tick, then voice, then pitch.
    const std::vector<ScoreNote>& notes(std::size_t staff_index) const;

    /// Inserts a note or rest and returns its assigned id, or
    /// `kInvalidEventId` if it was rejected.
    EventId insert_note(std::size_t staff_index, const ScoreNote& note);

    /// Inserts a chord: several pitches sharing a start tick and duration.
    /// Returns the ids assigned, in the order given.
    std::vector<EventId> insert_chord(std::size_t staff_index, Ticks start_tick,
                                      const Duration& duration,
                                      const std::vector<Pitch>& pitches,
                                      std::uint8_t voice = 0);

    /// Removes a note by id. Returns false if no such note exists.
    bool remove_note(EventId id);

    /// Re-inserts a note that was removed, keeping the id it had.
    ///
    /// This is what makes undo cheap at the host end. A plain re-insert
    /// would assign a fresh id, so the diff layer would see the whole
    /// passage as deleted and re-added, and everything the user attached to
    /// those events in Pro Tools would be lost - which is exactly what
    /// spec 13's event diff exists to prevent. Undoing a deletion has to
    /// restore the same events, not equivalent ones.
    ///
    /// Refuses an invalid note, an id already in use, or `kInvalidEventId`.
    bool restore_note(std::size_t staff_index, const ScoreNote& note);

    /// Replaces a note, keeping its id so a diff reports a modification
    /// rather than a delete and an add.
    bool modify_note(EventId id, const ScoreNote& replacement);

    /// Finds a note by id. Returns nullptr when absent.
    const ScoreNote* find_note(EventId id) const noexcept;

    /// Index of the staff holding a note, or SIZE_MAX when absent.
    std::size_t staff_index_of(EventId id) const noexcept;

    /// Notes overlapping the half-open range, on one staff.
    std::vector<ScoreNote> notes_in_range(std::size_t staff_index, Ticks from,
                                          Ticks to) const;

    // --- Markings -----------------------------------------------------------

    void add_dynamic(const DynamicMarking& marking);
    void add_hairpin(const Hairpin& hairpin);
    void add_slur(const Slur& slur);
    void add_tempo(const TempoMarking& marking);
    void add_text(const TextMarking& marking);

    const std::vector<DynamicMarking>& dynamics() const noexcept { return dynamics_; }
    const std::vector<Hairpin>& hairpins() const noexcept { return hairpins_; }
    const std::vector<Slur>& slurs() const noexcept { return slurs_; }
    const std::vector<TempoMarking>& tempos() const noexcept { return tempos_; }
    const std::vector<TextMarking>& texts() const noexcept { return texts_; }

    /// The dynamic in force at `tick` for a note in `voice` on `staff`, or
    /// mezzo-forte when none has been written - a score with no dynamics
    /// still has to play. The latest marking on the note's own staff; when
    /// its staff has none in force, the latest on any staff, which is what
    /// lets one marking between a piano's staves govern both hands.
    DynamicLevel dynamic_at(Ticks tick, std::uint8_t voice = 0, std::size_t staff = 0) const noexcept;

    /// The hairpin covering `tick`, or nullptr: one on the note's own
    /// staff first, then one on any staff.
    const Hairpin* hairpin_at(Ticks tick, std::uint8_t voice = 0, std::size_t staff = 0) const noexcept;

    /// True when `staff` has any dynamic marking or hairpin of its own in
    /// force at or before `tick`: the markings of a staff with none are
    /// the other staves', and a renderer sending controllers for the
    /// staff sends those.
    bool staff_has_dynamics(std::size_t staff, Ticks tick) const noexcept;

    /// Tempo in quarter notes per minute at `tick`.
    double tempo_at(Ticks tick) const noexcept;

    bool remove_dynamic(Ticks tick, std::uint8_t voice = 0, std::uint8_t staff = 0);
    bool remove_hairpin(Ticks start_tick, std::uint8_t voice = 0, std::uint8_t staff = 0);
    /// Removes the slur matching `slur` in every field. Matched whole rather
    /// than by range and voice, because two staves can carry slurs over the
    /// same range and removing one must not remove the other.
    bool remove_slur(const Slur& slur);
    bool remove_text(Ticks tick, std::uint8_t voice = 0);

    /// Removes a signature change. The change at tick 0 cannot be removed:
    /// a score always has a meter and a key in force from its start, which
    /// is what lets every lookup return a value rather than an optional.
    bool remove_time_signature(Ticks tick);
    bool remove_key_signature(Ticks tick);

    // --- Whole-score --------------------------------------------------------

    /// Last tick occupied by any note or marking. 0 for an empty score.
    Ticks end_tick() const noexcept;

    /// Total notes across every staff.
    std::size_t total_note_count() const noexcept;

    void clear();

    /// Monotonic counter, bumped on every mutation. Lets a UI or a renderer
    /// skip work when nothing has changed, without comparing the score.
    std::uint64_t revision() const noexcept { return revision_; }

private:
    EventId next_id() noexcept { return ++last_event_id_; }
    void touch() noexcept { ++revision_; }

    /// Keeps a staff's notes ordered by start tick, then voice, then pitch,
    /// so rendering and range queries never have to sort.
    void sort_staff(std::size_t staff_index);

    /// Raises this staff's bound to cover `note`. Never lowers it.
    void note_reaches(std::size_t staff_index, const ScoreNote& note);

    std::vector<Staff> staves_;
    std::vector<std::vector<ScoreNote>> staff_notes_;

    /// An upper bound on how long any one note on each staff sounds for.
    ///
    /// What lets `notes_in_range` stop scanning. The notes are sorted by
    /// start tick, so the *first* note that could reach into a range
    /// beginning at `from` cannot start before `from - longest`, and a
    /// binary search finds it. Without a bound there is nothing to search
    /// back to, because a whole note written in bar one overlaps bar one and
    /// nothing later, but only the duration says so.
    ///
    /// Deliberately an **upper bound** rather than the exact maximum: it
    /// climbs when a note is inserted or lengthened and is never lowered
    /// when one is removed or shortened. Recomputing it on removal would put
    /// the linear scan back exactly where it was taken out, and a bound that
    /// is too large only widens the search - it can never hide a note.
    std::vector<Ticks> longest_note_ticks_;

    std::vector<TimeSignatureChange> time_signatures_;
    std::vector<KeySignatureChange> key_signatures_;

    std::vector<DynamicMarking> dynamics_;
    std::vector<Hairpin> hairpins_;
    std::vector<Slur> slurs_;
    std::vector<TempoMarking> tempos_;
    std::vector<TextMarking> texts_;

    EventId last_event_id_ = kInvalidEventId;
    std::uint64_t revision_ = 0;
};

}  // namespace vcc::notation
