// Stems (Main Spec §16): which tracks each stem prints, the default stems a
// project starts with, and the checks a deliverable needs - every track in
// exactly one stem, so the stems add back up to the mix.
#pragma once

#include <string>
#include <vector>

#include "vcc/project/Project.h"

namespace vcc::project {

/// The tracks a stem prints: its track sources, plus every track routed
/// into one of its bus sources (directly or through other buses). Only
/// tracks that are part of deliverables (Project::export_tracks semantics:
/// reference/temp tracks are out unless opted in). In project order.
std::vector<const Track*> stem_tracks(const StemDef& stem, const ProjectData& data);

/// Stems for a project that has none: one per group bus that tracks feed
/// ("Strings Bus" -> "Strings"), then the tracks routed straight to the
/// master grouped by instrument family ("Woodwinds", "Synth", "Audio"...).
/// `family_of` names a track's family (the instrument registry knows it);
/// empty falls back to Track::family. Ids are "stem-1", "stem-2"...
std::vector<StemDef> default_stems(const ProjectData& data,
                                   const std::function<std::string(const Track&)>& family_of = {});

struct StemCoverage {
    std::vector<std::string> missing;  ///< Deliverable tracks in no stem (by name).
    std::vector<std::string> doubled;  ///< Tracks in more than one stem (by name).
    std::vector<std::string> empty;    ///< Stems that print no track (by name).
    bool complete() const noexcept { return missing.empty() && doubled.empty(); }
};
/// Whether the stems add back up to the mix.
StemCoverage stem_coverage(const std::vector<StemDef>& stems, const ProjectData& data);

}  // namespace vcc::project
