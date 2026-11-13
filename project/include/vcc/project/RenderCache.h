// The rendered cache (Main Spec §8): a track's MIDI kept between edits and
// re-rendered by dirty time range, so an edit costs its own size.
//
// Every edit used to re-render every track whole: the playback engine on
// each document change, the Sequencer twice per track. The cache keeps the
// last score render of each track and the score it came from. On the next
// call it compares the scores note by note (by stable id) and re-renders
// only the window the changes reach:
//   * the changed notes' bars, one bar either side (for grace notes, which
//     sound before their beat);
//   * widened until no tie chain crosses its edges (a chain renders as one
//     note from its first);
//   * to the end of the score from a changed dynamic or hairpin (a level
//     holds until the next).
// Events of notes in the window, and marking events at ticks in it, are
// replaced; everything else is kept. A change the renderer reads globally
// (staves, transposition, the expression map, the channel, the version)
// renders the track whole. Performance overrides and regions are applied
// on top, and only when they or the score render changed.
//
// The result is identical to `render_track`: the same events, in the same
// canonical order (tests compare them over long runs of random edits).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vcc/project/Render.h"

namespace vcc::project {

struct RenderStats {
    enum class Kind {
        Unchanged,  ///< Nothing changed: the last result, as it was.
        Refinish,   ///< The score render was reused; overrides or regions changed.
        Partial,    ///< A window of the score re-rendered.
        Full,       ///< The whole score rendered.
    };
    Kind kind = Kind::Full;
    Ticks from = 0, to = 0;            ///< The re-rendered window (Partial).
    std::size_t events_rendered = 0;   ///< Events the renderer produced for this call.
    std::size_t events_total = 0;      ///< Events in the track's score render.
};

const char* to_string(RenderStats::Kind kind) noexcept;

class RenderCache {
public:
    /// What `render_track(track, performance)` returns, from the cache.
    /// Valid until the next call for the same track, or `forget`/`clear`.
    const RenderedTrack& render(const Track& track, bool performance = true);

    /// How the last call was served.
    const RenderStats& last() const noexcept { return last_; }

    /// Drops a track's entry, or every track not in `keep`.
    void forget(const std::string& track_id);
    void retain(const std::vector<std::string>& keep);
    void clear() { entries_.clear(); }
    std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        std::string version_id;
        std::string map_name;
        int channel = -1;
        notation::Score score;
        notation::RenderResult raw;
        std::vector<MidiRegion> regions;
        std::map<notation::EventId, NoteOverride> performance;
        std::optional<RenderedTrack> finished[2];  ///< [0] as written, [1] performed.
    };
    std::map<std::string, Entry> entries_;
    RenderStats last_;
};

}  // namespace vcc::project
