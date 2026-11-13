#include "vcc/project/Performance.h"

#include <algorithm>

#include "vcc/project/Render.h"

namespace vcc::project {

namespace {
constexpr Ticks kMaxShift = 2 * kTicksPerQuarter;

std::uint64_t mix(std::uint64_t x) noexcept
{
    // splitmix64: a fixed, portable hash, so a seed means the same thing on
    // every platform and in every release.
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/// A triangular value in [-1, 1]: small departures are likelier than large ones.
double jitter(std::uint64_t seed, notation::EventId id, std::uint64_t channel) noexcept
{
    const std::uint64_t h = mix(mix(seed ^ (channel * 0xD1B54A32D192ED03ull)) ^ id);
    const double a = static_cast<double>(h >> 40) / static_cast<double>(1ull << 24);
    const double b = static_cast<double>((h >> 16) & 0xFFFFFFull) / static_cast<double>(1ull << 24);
    return a - b;
}
}  // namespace

NoteOverride clamp_override(const NoteOverride& o) noexcept
{
    NoteOverride c;
    c.onset = std::clamp<Ticks>(o.onset, -kMaxShift, kMaxShift);
    c.length = std::clamp<Ticks>(o.length, -kMaxShift, 8 * kTicksPerQuarter);
    c.velocity = std::clamp(o.velocity, -127, 127);
    c.cc = std::clamp(o.cc, -127, 127);
    return c;
}

HumanizeSettings humanize_preset(const std::string& name)
{
    HumanizeSettings s;
    if (name == "natural") {
        s.timing = 70;
        s.velocity = 9;
        s.length = 60;
    } else if (name == "loose") {
        s.timing = 150;
        s.velocity = 15;
        s.length = 160;
    }
    return s;
}

NoteOverride humanized(const NoteOverride& current, notation::EventId id, const HumanizeSettings& s) noexcept
{
    NoteOverride o = current;
    if (s.timing > 0) o.onset = static_cast<Ticks>(jitter(s.seed, id, 1) * static_cast<double>(s.timing));
    if (s.length > 0) o.length = static_cast<Ticks>(jitter(s.seed, id, 2) * static_cast<double>(s.length));
    if (s.velocity > 0) o.velocity = static_cast<int>(jitter(s.seed, id, 3) * static_cast<double>(s.velocity));
    return clamp_override(o);
}

std::vector<notation::EventId> performable_notes(const Track& track)
{
    std::vector<notation::EventId> ids;
    for (auto id : render_track(track, false).note_ids)
        if (id != notation::kInvalidEventId) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::size_t prune_performance(TrackVersion& version)
{
    std::size_t dropped = 0;
    for (auto it = version.performance.begin(); it != version.performance.end();) {
        if (it->second.empty() || !version.score.find_note(it->first)) {
            it = version.performance.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    return dropped;
}

}  // namespace vcc::project
