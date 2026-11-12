#include "vcc/notation/EventDiff.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace vcc::notation {
namespace {

/// Identity of an event for matching purposes.
///
/// A source note id plus the kind of event and the note or controller
/// number. Position and value are deliberately excluded: those are what a
/// modification changes, so including them would turn every move into a
/// delete and an add.
struct EventKey {
    EventId source_id;
    std::uint8_t type;
    std::uint8_t role;
    /// Only used for events with no source note. An event that belongs to a
    /// note is identified by that note and its role, so changing the note's
    /// pitch reports a modification rather than a deletion and an addition -
    /// which is what lets the host move the existing MIDI note and keep
    /// whatever the user attached to it.
    std::uint8_t data1;
    std::uint8_t channel;
    /// Only used for events with no source note, where position is the only
    /// thing distinguishing one controller message from the next.
    Ticks tick;

    bool operator<(const EventKey& other) const noexcept
    {
        return std::tie(source_id, type, role, data1, channel, tick)
             < std::tie(other.source_id, other.type, other.role, other.data1, other.channel,
                        other.tick);
    }
};

EventKey key_for(const TimedMidiEvent& event) noexcept
{
    EventKey key;
    const bool belongs_to_a_note = event.source_id != kInvalidEventId;

    key.source_id = event.source_id;
    key.type = static_cast<std::uint8_t>(event.event.type);
    key.role = static_cast<std::uint8_t>(event.role);
    key.channel = event.event.channel;

    // An event belonging to a note is identified by that note and by what
    // it is for, so it can move in time, change pitch or change velocity
    // and still be the same event. One belonging to a marking has no note
    // to anchor it, so its position and controller number are part of its
    // identity.
    key.data1 = belongs_to_a_note ? 0 : event.event.data1;
    key.tick = belongs_to_a_note ? 0 : event.tick;
    return key;
}

bool same_content(const TimedMidiEvent& a, const TimedMidiEvent& b) noexcept
{
    return a.tick == b.tick && a.event.type == b.event.type
        && a.event.channel == b.event.channel && a.event.data1 == b.event.data1
        && a.event.data2 == b.event.data2 && a.event.bend == b.event.bend;
}

}  // namespace

const char* to_string(DiffKind kind) noexcept
{
    switch (kind) {
        case DiffKind::Added:    return "added";
        case DiffKind::Removed:  return "removed";
        case DiffKind::Modified: break;
    }
    return "modified";
}

void EventDiff::clear()
{
    changes.clear();
    added = 0;
    removed = 0;
    modified = 0;
    unchanged = 0;
}

EventDiff diff_events(const RenderResult& previous, const RenderResult& current)
{
    EventDiff diff;

    // A note can legitimately produce several events with the same key - a
    // repeated keyswitch on the same note, for instance - so each key maps
    // to a list and matches are consumed in order.
    std::map<EventKey, std::vector<const TimedMidiEvent*>> previous_by_key;
    for (const TimedMidiEvent& event : previous.events)
        previous_by_key[key_for(event)].push_back(&event);

    std::map<EventKey, std::size_t> consumed;

    for (const TimedMidiEvent& event : current.events) {
        const EventKey key = key_for(event);
        const auto found = previous_by_key.find(key);

        const std::size_t already_used = consumed[key];
        if (found == previous_by_key.end() || already_used >= found->second.size()) {
            EventChange change;
            change.kind = DiffKind::Added;
            change.event = event;
            diff.changes.push_back(change);
            ++diff.added;
            continue;
        }

        const TimedMidiEvent& before = *found->second[already_used];
        ++consumed[key];

        if (same_content(before, event)) {
            ++diff.unchanged;
            continue;
        }

        EventChange change;
        change.kind = DiffKind::Modified;
        change.previous = before;
        change.event = event;
        diff.changes.push_back(change);
        ++diff.modified;
    }

    // Anything in the previous render that was never matched has gone.
    for (const auto& entry : previous_by_key) {
        const std::size_t used = consumed[entry.first];
        for (std::size_t index = used; index < entry.second.size(); ++index) {
            EventChange change;
            change.kind = DiffKind::Removed;
            change.previous = *entry.second[index];
            diff.changes.push_back(change);
            ++diff.removed;
        }
    }

    // Ordered by position so the host adapter can apply the changes in a
    // single forward pass.
    std::stable_sort(diff.changes.begin(), diff.changes.end(),
                     [](const EventChange& a, const EventChange& b) {
                         const Ticks a_tick =
                             a.kind == DiffKind::Removed ? a.previous.tick : a.event.tick;
                         const Ticks b_tick =
                             b.kind == DiffKind::Removed ? b.previous.tick : b.event.tick;
                         return a_tick < b_tick;
                     });

    return diff;
}

ScoreSynchronizer::ScoreSynchronizer() = default;

ScoreSynchronizer::ScoreSynchronizer(MidiRenderer renderer) : renderer_(std::move(renderer)) {}

bool ScoreSynchronizer::is_up_to_date(const Score& score) const noexcept
{
    return has_rendered_once_ && score.revision() == last_revision_;
}

EventDiff ScoreSynchronizer::update(const Score& score)
{
    const RenderResult current = renderer_.render(score);

    EventDiff diff;
    if (!has_rendered_once_) {
        // Nothing is in the host yet, so everything is an addition.
        diff.changes.reserve(current.events.size());
        for (const TimedMidiEvent& event : current.events) {
            EventChange change;
            change.kind = DiffKind::Added;
            change.event = event;
            diff.changes.push_back(change);
        }
        diff.added = current.events.size();
    } else {
        diff = diff_events(last_, current);
    }

    last_ = current;
    has_rendered_once_ = true;
    last_revision_ = score.revision();

    events_sent_ += diff.size();
    rewrite_equivalent_ += current.events.size();
    return diff;
}

void ScoreSynchronizer::reset()
{
    last_.clear();
    has_rendered_once_ = false;
    last_revision_ = 0;
    events_sent_ = 0;
    rewrite_equivalent_ = 0;
}

}  // namespace vcc::notation
