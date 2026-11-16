#include "vcc/session/Retrospective.h"

#include <algorithm>
#include <map>

namespace vcc::session {

std::optional<project::MidiRegion> capture_performance(const std::vector<LiveMidi>& events, const TempoMap& tempo,
                                                       Ticks at, double gap_seconds)
{
    if (events.empty()) return std::nullopt;
    // The last performance: back from the end to the last long pause.
    std::size_t first = events.size() - 1;
    while (first > 0 && events[first].wall_seconds - events[first - 1].wall_seconds <= gap_seconds) --first;
    const std::vector<LiveMidi> take(events.begin() + static_cast<std::ptrdiff_t>(first), events.end());

    // Along with the transport when most of it carries ticks; else free time.
    const auto with_ticks = std::count_if(take.begin(), take.end(), [](const LiveMidi& e) { return e.tick >= 0; });
    const bool along = with_ticks * 2 > static_cast<std::ptrdiff_t>(take.size());
    const double t0 = take.front().wall_seconds;
    const double start_seconds = tempo.seconds_at(at);
    auto tick_of = [&](const LiveMidi& e) -> Ticks {
        if (along && e.tick >= 0) return e.tick;
        if (along) {
            // A stray event without a position: nearest neighbour's tick by wall time.
            const LiveMidi* best = nullptr;
            for (const auto& x : take)
                if (x.tick >= 0 && (!best || std::abs(x.wall_seconds - e.wall_seconds) < std::abs(best->wall_seconds - e.wall_seconds)))
                    best = &x;
            return best ? tempo.tick_at_seconds(tempo.seconds_at(best->tick) + (e.wall_seconds - best->wall_seconds)) : at;
        }
        return tempo.tick_at_seconds(start_seconds + (e.wall_seconds - t0));
    };

    project::MidiRegion r;
    r.source = "capture";
    std::map<int, std::pair<Ticks, int>> open;  // channel*128+pitch -> (start, velocity)
    Ticks last = 0;
    for (const auto& e : take) {
        const Ticks tk = std::max<Ticks>(0, tick_of(e));
        last = std::max(last, tk);
        const int type = e.status & 0xF0, ch = e.status & 0x0F;
        const int key = ch * 128 + (e.data1 & 127);
        if (type == 0x90 && e.data2 > 0) {
            if (auto it = open.find(key); it != open.end()) {
                r.notes.push_back({it->second.first, std::max<Ticks>(1, tk - it->second.first), e.data1 & 127, it->second.second, ch, {}});
                open.erase(it);
            }
            open[key] = {tk, e.data2};
        } else if (type == 0x80 || type == 0x90) {
            if (auto it = open.find(key); it != open.end()) {
                r.notes.push_back({it->second.first, std::max<Ticks>(1, tk - it->second.first), e.data1 & 127, it->second.second, ch, {}});
                open.erase(it);
            }
        } else if (type == 0xB0) {
            r.controllers.push_back({tk, ch, e.data1 & 127, e.data2 & 127});
        } else if (type == 0xE0) {
            r.controllers.push_back({tk, ch, kPitchBendController, (e.data2 << 7) | e.data1});
        } else if (type == 0xD0) {
            r.controllers.push_back({tk, ch, kChannelPressureController, e.data1 & 127});
        }
    }
    for (const auto& [key, s] : open) r.notes.push_back({s.first, std::max<Ticks>(1, last - s.first), key % 128, s.second, key / 128, {}});
    if (r.notes.empty()) return std::nullopt;
    std::sort(r.notes.begin(), r.notes.end(), [](const NoteEvent& a, const NoteEvent& b) { return a.start < b.start; });
    r.start = r.notes.front().start;
    for (const auto& c : r.controllers) r.start = std::min(r.start, c.tick);
    r.end = r.start + 1;
    for (const auto& n : r.notes) r.end = std::max(r.end, n.end());
    for (const auto& c : r.controllers) r.end = std::max(r.end, c.tick + 1);
    return r;
}

}  // namespace vcc::session
