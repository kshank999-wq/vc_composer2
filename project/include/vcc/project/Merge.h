// Collaboration (Main Spec §15): three-way merge of a shared project.
//
// Push/pull with merge: each collaborator works on their own copy; Sync
// brings in what others pushed since the copy's base revision. The merge
// works on the saved project document, item by item: tracks, versions,
// notes, regions, buses, markers... are matched by their ids (which never
// collide in a shared project: Score::set_site, IdAllocator::set_site), and
// fields one by one. A change made on one side only is taken; a change made
// on both sides to the same value is taken once; only the same thing changed
// differently on both sides - or changed on one side and deleted on the
// other - is a conflict. Conflicts are never resolved silently: the merge
// keeps "ours" at each one and reports it, in words, so the person syncing
// chooses (MergeResult::resolve).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vcc/core/Json.h"

namespace vcc::project {

struct MergeConflict {
    /// Where, as a path of keys and item ids: "tracks/trk-0003/versions/
    /// ver-0004/score/staves/0/notes/7/step".
    std::string path;
    /// In words: "Violins I, bar 3: a note's pitch".
    std::string description;
    enum class Kind { Changed, ChangedAndDeleted } kind = Kind::Changed;
    /// The three values (nullopt: absent on that side).
    std::optional<json::Value> base, ours, theirs;
};

struct MergeResult {
    json::Value merged;  ///< With "ours" at every conflict.
    std::vector<MergeConflict> conflicts;
    /// Changes taken from each side (for the summary line).
    int from_ours = 0, from_theirs = 0;
    bool clean() const noexcept { return conflicts.empty(); }
    /// The merged document with "theirs" taken at the conflicts chosen
    /// (`take_theirs[i]` for conflicts[i]).
    json::Value resolve(const std::vector<bool>& take_theirs) const;
};

/// Merges two descendants of `base` (project documents, Project::to_json).
MergeResult merge_projects(const json::Value& base, const json::Value& ours, const json::Value& theirs);

}  // namespace vcc::project
