// Cue management for film and episodic work (Main Spec §14): cue times and
// spotting drift, dashboard filters, overlaps, cue codes, cue sheet and
// spotting-note exports, and bounce file naming.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vcc/project/Project.h"

namespace vcc::cues {

struct CueTimes {
    double start_seconds = 0.0;  ///< Project time of the music range.
    double end_seconds = 0.0;
    std::string in_tc;           ///< Timecode of the music range.
    std::string out_tc;
    std::string duration;        ///< "1:23" (m:ss).
};
CueTimes times_of(const project::Cue& cue, const project::ProjectData& d);

/// How far the music range has moved from where the cue was spotted, in
/// frames (music start minus spot-in). nullopt when the cue was not spotted.
std::optional<std::int64_t> spot_drift_frames(const project::Cue& cue, const project::ProjectData& d);

struct CueFilter {
    std::string status;   ///< Empty = any.
    std::string reel_id;  ///< Empty = any.
    std::string query;    ///< Code, title, description, notes, composer.
    bool overdue_only = false;
    std::string today;    ///< ISO date used for "overdue".
};

/// Cues matching a filter, in reel order then by start.
std::vector<const project::Cue*> list(const project::ProjectData& d, const CueFilter& filter = {});

/// Pairs of cue ids whose music ranges overlap (allowed - transitions - but shown).
std::vector<std::pair<std::string, std::string>> overlaps(const project::ProjectData& d);

/// The next free cue code in a reel: reel number, "M", sequence ("1M1",
/// "1M2", ... "2M1"); "M1", "M2" when no reel is given.
std::string next_code(const project::ProjectData& d, const std::string& reel_id);

/// Whether a cue is past its deadline and not yet approved or locked.
bool overdue(const project::Cue& cue, const std::string& today);

/// Cue sheet (CSV): code, title, reel, scene, in, out, duration, usage,
/// composer, status, revision. Fields are quoted per RFC 4180.
std::string cue_sheet_csv(const project::ProjectData& d);

/// Spotting notes: each cue with its range, description, notes and the hits
/// and sync points inside it, as plain text for the director and editor.
std::string spotting_notes(const project::ProjectData& d);

/// A bounce file name from a preset with tokens {project} {production}
/// {reel} {code} {title} {rev} {status} {date}; characters that are unsafe
/// in file names are replaced. Default preset: "{production}_{code}_{title}_v{rev}".
std::string render_name(const std::string& preset, const project::Cue& cue, const project::ProjectData& d,
                        const std::string& date = {});

}  // namespace vcc::cues
