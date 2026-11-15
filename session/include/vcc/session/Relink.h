// Relink missing media (Main Spec §16 "Project portability"): when a project
// opens on another machine, or its audio and picture were moved, find the
// files again by searching folders, and point every reference at them in one
// undoable step.
//
// A missing file is matched by name (case ignored, which also suits
// Windows and macOS volumes). Among several files of that name, the one whose
// folders also match the stored path ("Audio Files/Vln_01.wav") ranks first;
// when none is clearly best the choice is left to the user. A media file
// with the same name but another extension (a .wav converted to .aif) is
// offered after exact names, never chosen on its own.
// Finding one file is a good hint where the rest went, so one search covers
// every missing file at once.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "vcc/project/Project.h"
#include "vcc/session/Package.h"

namespace vcc::session::relink {

/// The media the project uses that cannot be found now.
std::vector<package::Asset> missing(const project::ProjectData& data, const std::string& project_path);

struct Candidate {
    std::string path;       ///< Absolute, generic separators.
    int score = 0;          ///< Higher is better.
    bool same_name = true;  ///< False: the same name with another extension.
    std::uintmax_t bytes = 0;
};

struct Match {
    std::string stored;                 ///< The missing reference, as stored.
    package::Asset::Kind kind = package::Asset::Kind::Audio;
    std::vector<Candidate> candidates;  ///< Best first; empty when not found.
    /// The best candidate when it is clearly best: one exact-name file, or
    /// one that beats the others on its folders.
    std::string chosen;
    bool ambiguous() const noexcept { return chosen.empty() && !candidates.empty(); }
};

struct SearchResult {
    std::vector<Match> matches;  ///< One per missing file, in its order.
    int files_scanned = 0;
    bool cancelled = false;
    int found() const noexcept;      ///< Matches with a chosen file.
    int ambiguous() const noexcept;  ///< Matches with candidates but no clear best.
};

/// Called with the files scanned so far; return false to cancel.
using Progress = std::function<bool(int files_scanned)>;

/// Searches `folders` (recursively, links not followed, hidden folders
/// skipped) for the `missing` files.
SearchResult search(const std::vector<package::Asset>& missing, const std::vector<std::string>& folders,
                    const Progress& progress = {});

/// How `found` is stored: relative to the project's folder when it is inside
/// it (the project stays portable), else absolute.
std::string stored_path(const std::string& found, const std::string& project_path);

/// Points every reference to each key (a stored path) at its value (a file
/// found), in clips of every version of every track (takes too) and in
/// picture cuts. Returns the references changed.
int apply(project::ProjectData& data, const std::map<std::string, std::string>& relinks,
          const std::string& project_path);

}  // namespace vcc::session::relink
