// MIDI -> notation (Main Spec §8): carrying what was played or edited as
// MIDI back into the score.
//
// Two ways in:
//
//   * A piano-roll edit to a score note. The edit is written into the score
//     when the result is notation (a note moved or stretched to a grid
//     position, a new pitch); otherwise it is kept as a performance override
//     (Performance.h), so the sound changes and the page does not.
//   * A MIDI region (played, drawn, or generated) notated into the score:
//     quantized, spelled in the written key, split at barlines and tied,
//     with the played timing and velocities optionally kept as overrides so
//     it sounds exactly as it did.
//
// Neither changes anything by itself. Each returns a plan: one or more
// complete outcomes. When the edit means one thing there is one option;
// when it is ambiguous (a triplet or a near-straight rhythm, a note that
// now overlaps another of its voice, an enharmonic spelling) the options
// are the readings, recommended first, and the app asks. The chosen
// option is applied as one undo step.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "vcc/notation/Score.h"
#include "vcc/project/Project.h"

namespace vcc::session {

/// Where an edited score note should now sound: sounding pitch and the
/// performed position, as the piano roll shows it.
struct NoteTarget {
    Ticks start = 0;
    Ticks duration = 0;
    int pitch = 60;
};

/// One complete reading of an edit.
struct NotateOption {
    std::string id;      ///< "notate", "triplet", "straight", "override", "voice", "replace", "sharp", "flat"...
    std::string label;   ///< "Write it as a triplet"
    std::string detail;  ///< What it does, for the prompt.
    /// The active version's score and overrides afterwards.
    notation::Score score;
    std::map<notation::EventId, project::NoteOverride> performance;
    /// Regions this option removes (their notes are now in the score).
    std::vector<std::string> removed_regions;
    bool changes_notation = false;
};

struct NotatePlan {
    /// Empty when nothing needs asking; otherwise "tuplet", "overlap",
    /// "spelling" or "placement".
    std::string ambiguity;
    std::string question;
    /// Recommended first. Empty with `error` set when nothing can be done.
    std::vector<NotateOption> options;
    std::string error;
    /// The score revision the plan was made against; applying refuses a
    /// plan whose version has changed since.
    std::uint64_t base_revision = 0;
    bool ok() const noexcept { return error.empty() && !options.empty(); }
};

/// Plans a piano-roll edit of score note `note` (a tie chain's first note)
/// on the track's active version.
NotatePlan plan_note_edit(const project::Track& track, notation::EventId note, const NoteTarget& target);

struct RegionNotateSettings {
    notation::NoteValue grid = notation::NoteValue::Sixteenth;
    bool allow_triplets = true;
    /// Keep the played timing and velocity as performance overrides, so the
    /// notated passage sounds as the region did.
    bool keep_feel = true;
};

/// Plans notating MIDI region `region_id` into the active version's score
/// (its first staff), removing the region. Asks where to put it when it
/// lands on written notes, and offers a straight reading when the
/// quantizer heard triplets.
NotatePlan plan_notate_region(const project::Track& track, const std::string& region_id,
                              const RegionNotateSettings& settings = {});

/// The written span of a score note and the notes tied on from it:
/// {start, end}, or {0, 0} for an unknown note.
std::pair<Ticks, Ticks> written_span(const notation::Score& score, notation::EventId note);

/// `written_span` for every note that starts a tie chain (or stands alone),
/// in one pass over the score.
std::map<notation::EventId, std::pair<Ticks, Ticks>> written_spans(const notation::Score& score);

/// What a plan option did, in a sentence for the status bar.
std::string describe(const NotateOption& option);

}  // namespace vcc::session
