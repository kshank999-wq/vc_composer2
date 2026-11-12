// Copyright (c) VC Composer. All rights reserved.
//
// Event-diff synchronization (spec 02 section 13).
//
// Spec 13 is explicit: "Use an event-diff synchronization layer rather than
// rewriting the entire MIDI region after every edit." That matters for more
// than efficiency. Rewriting a whole region on every keystroke destroys
// anything the user changed in the host - hand-drawn automation, nudged
// note positions, edits made in Pro Tools rather than in notation - and it
// makes undo history in the host useless.
//
// The diff works because every rendered event carries the stable id of the
// score note that produced it, and because rendering is deterministic: the
// same score renders identically every time, so any difference between two
// renders is a real edit.
#pragma once

#include <cstdint>
#include <vector>

#include "vcc/notation/MidiRenderer.h"

namespace vcc::notation {

/// What happened to one event between two renders.
enum class DiffKind : std::uint8_t {
    Added = 0,
    Removed,
    /// Same source note and same kind of event, but a changed position or
    /// value. Reported separately from a remove-and-add pair so the host
    /// adapter can move an existing event instead of deleting and
    /// recreating it - which is what preserves anything attached to it.
    Modified,
};

const char* to_string(DiffKind kind) noexcept;

/// One change to apply to the host.
struct EventChange {
    DiffKind kind = DiffKind::Added;

    /// The event as it should end up. Unused for `Removed`.
    TimedMidiEvent event{};

    /// The event as it was. Unused for `Added`.
    TimedMidiEvent previous{};
};

/// The complete set of changes between two renders.
struct EventDiff {
    std::vector<EventChange> changes;

    std::size_t added = 0;
    std::size_t removed = 0;
    std::size_t modified = 0;

    /// Events identical in both renders, and so left alone. The number that
    /// matters: it is what a whole-region rewrite would have touched
    /// needlessly.
    std::size_t unchanged = 0;

    bool empty() const noexcept { return changes.empty(); }
    std::size_t size() const noexcept { return changes.size(); }

    void clear();
};

/// Computes the changes needed to turn `previous` into `current`.
///
/// Events are matched by source note id and event kind, so a note whose
/// velocity changed is one modification rather than a deletion and an
/// insertion. Events with no source note - a hairpin's controller
/// messages - are matched by position and controller number instead.
EventDiff diff_events(const RenderResult& previous, const RenderResult& current);

/// Tracks the last render of a score so that successive edits produce
/// incremental changes.
///
/// Typical use: the UI edits the score, calls `update`, and hands the
/// returned diff to the host adapter. Nothing else has to remember what the
/// host was last told.
class ScoreSynchronizer {
public:
    ScoreSynchronizer();
    explicit ScoreSynchronizer(MidiRenderer renderer);

    /// Re-renders the score and returns what changed since the last call.
    /// The first call reports every event as an addition, which is correct:
    /// the host has nothing yet.
    EventDiff update(const Score& score);

    /// True when the score's revision has not changed since the last
    /// update, so no work is needed at all.
    bool is_up_to_date(const Score& score) const noexcept;

    /// Forgets the last render, so the next update reports everything as
    /// new. Used after the host region has been cleared or the binding has
    /// changed to a different track.
    void reset();

    const RenderResult& last_render() const noexcept { return last_; }

    MidiRenderer& renderer() noexcept { return renderer_; }
    const MidiRenderer& renderer() const noexcept { return renderer_; }

    /// Total events sent to the host since the last reset, and the total a
    /// whole-region rewrite would have sent. The ratio is the concrete
    /// benefit spec 13 asks for.
    std::uint64_t events_sent() const noexcept { return events_sent_; }
    std::uint64_t events_a_rewrite_would_have_sent() const noexcept
    {
        return rewrite_equivalent_;
    }

private:
    MidiRenderer renderer_;
    RenderResult last_;
    bool has_rendered_once_ = false;
    std::uint64_t last_revision_ = 0;
    std::uint64_t events_sent_ = 0;
    std::uint64_t rewrite_equivalent_ = 0;
};

}  // namespace vcc::notation
