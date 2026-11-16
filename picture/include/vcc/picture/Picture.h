// Picture sync and hit-point tools (Main Spec §13).
//
// Time model:
//   * project time is seconds from the project's start timecode
//     (ProjectData::start_timecode at ProjectData::frame_rate);
//   * a picture version's frame f carries timecode start_timecode + f at the
//     picture's own rate, and plays at the project time of that timecode, plus
//     the user-approved offset;
//   * markers (hits, sync points, streamers, punches) live in project time, so
//     a tempo change never moves a picture event; the music moves to them.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vcc/core/Time.h"
#include "vcc/project/Project.h"

namespace vcc::picture {

/// Seconds of a timecode label, counted in frames of `rate` from 00:00:00:00.
std::optional<double> timecode_seconds(const std::string& timecode, FrameRate rate);

/// Project time of a timecode label (at the project's frame rate).
std::optional<double> project_seconds_for_timecode(const project::ProjectData& d, const std::string& timecode);
/// Timecode label of a project time (at the project's frame rate).
std::string project_timecode(const project::ProjectData& d, double project_seconds);

/// Project time of a picture frame, and the picture frame shown at a project
/// time (floor; may be negative or past the end, meaning "no picture").
double project_seconds_for_frame(const project::ProjectData& d, const project::PictureVersion& p, std::int64_t frame);
std::int64_t frame_at(const project::ProjectData& d, const project::PictureVersion& p, double project_seconds);
/// Picture time (seconds from the movie's first frame) at a project time.
double picture_seconds_at(const project::ProjectData& d, const project::PictureVersion& p, double project_seconds);

/// True when the picture's rate differs from the project's: timecode will
/// not line up frame for frame, and the UI says so.
bool rate_mismatch(const project::ProjectData& d, const project::PictureVersion& p);

/// Where a picture event falls in the music.
struct MusicalPosition {
    Ticks tick = 0;               ///< Exact position.
    int bar = 1;
    double beat = 1.0;            ///< 1-based, fractional.
    Ticks nearest_beat_tick = 0;  ///< The nearest written beat.
    double offset_ms = 0.0;       ///< Event minus nearest beat, in milliseconds.
    double offset_frames = 0.0;   ///< The same in frames at the project rate.
};
MusicalPosition locate_in_music(const project::ProjectData& d, double project_seconds);

/// Tempo fit (hit-point tool): replaces the tempo between `from_tick` and
/// `target_tick` with one constant tempo so that `target_tick` sounds exactly
/// at `target_seconds`. Everything before `from_tick` is untouched; the tempo
/// in force after `target_tick` is kept. Fails (nullopt, reason in `error`)
/// when the required tempo is outside 20-400 bpm or the range is empty.
std::optional<TempoMap> fit_tempo_to_hit(const TempoMap& tempo, Ticks from_tick, Ticks target_tick,
                                         double target_seconds, std::string* error = nullptr);

/// Film footage (Main Spec §13 "feet+frames"): frames counted at the
/// picture rate from project time 0, 16 to the foot for 35mm 4-perf, 40 for
/// 16mm. "90+08"; negative times get a leading "-".
std::string feet_frames(double project_seconds, FrameRate rate, int frames_per_foot = 16);
/// Seconds of a "90+08" label (also "90+8", "90"); nullopt when it is not one.
std::optional<double> feet_frames_seconds(const std::string& text, FrameRate rate, int frames_per_foot = 16);

/// Tempo finder (hit points): the steady tempos at which picture events,
/// counted from a start, fall on the beat grid.
struct TempoSearch {
    double min_bpm = 60.0;
    double max_bpm = 180.0;
    Ticks grid = kTicksPerQuarter;  ///< What a hit must land on: a quarter, an eighth (kTicksPerQuarter / 2)...
    double tolerance_frames = 1.0;  ///< How near counts as on.
    double fps = 24.0;
    std::size_t max_results = 8;
};
struct TempoCandidate {
    double bpm = 120.0;
    int hits_on = 0;                  ///< Within the tolerance.
    double worst_frames = 0.0;        ///< Largest error of the hits on.
    std::vector<double> error_frames; ///< Per hit, in order: hit minus its grid point, in frames.
    std::vector<Ticks> hit_ticks;     ///< Per hit: its grid point counted from the start; -1 for a hit
                                      ///< at or before the start (ignored).
};
/// Best first: most hits on, then the smallest errors. Each tempo is the
/// least-squares fit of its hits, so the errors are as small as one steady
/// tempo can make them; near-identical tempos are reported once.
std::vector<TempoCandidate> find_tempos(double start_seconds, const std::vector<double>& hit_seconds,
                                        const TempoSearch& search);

/// Streamer wipe progress 0..1 while `now` is within the streamer leading to
/// the marker; nullopt otherwise.
std::optional<double> streamer_progress(const project::Marker& marker, double now_seconds);
/// Whether the punch (a short flash at the marker) is showing.
bool punch_visible(const project::Marker& marker, double now_seconds, double punch_seconds = 2.0 / 24.0);

/// Compares two cuts from per-step frame signatures (64-bit average hashes,
/// one every `step_seconds` of picture time) and returns the stretches that
/// differ, merged, in the new cut's project time. `bits` is the Hamming
/// distance above which two frames count as different.
std::vector<project::FlaggedRegion> changed_regions(const std::vector<std::uint64_t>& old_signatures,
                                                    const std::vector<std::uint64_t>& new_signatures,
                                                    double step_seconds, double new_cut_start_project_seconds,
                                                    int bits = 10);

}  // namespace vcc::picture
