#include "vcc/picture/Picture.h"

#include <algorithm>
#include <bitset>
#include <cmath>

namespace vcc::picture {

std::optional<double> timecode_seconds(const std::string& text, FrameRate rate)
{
    auto tc = Timecode::parse(text);
    if (!tc) return std::nullopt;
    auto frames = timecode_to_frames(*tc, rate);
    if (!frames) return std::nullopt;
    return frames_to_seconds(*frames, rate);
}

std::optional<double> project_seconds_for_timecode(const project::ProjectData& d, const std::string& tc)
{
    auto at = timecode_seconds(tc, d.frame_rate);
    auto origin = timecode_seconds(d.start_timecode, d.frame_rate);
    if (!at || !origin) return std::nullopt;
    return *at - *origin;
}

std::string project_timecode(const project::ProjectData& d, double seconds)
{
    const double origin = timecode_seconds(d.start_timecode, d.frame_rate).value_or(0.0);
    const std::int64_t frames = seconds_to_frames(origin + seconds, d.frame_rate);
    return frames_to_timecode(frames, d.frame_rate).to_string(d.frame_rate);
}

namespace {
double picture_origin(const project::ProjectData& d, const project::PictureVersion& p)
{
    // Project time of the movie's first frame.
    const double start = timecode_seconds(p.start_timecode, p.rate).value_or(0.0);
    const double origin = timecode_seconds(d.start_timecode, d.frame_rate).value_or(0.0);
    return start - origin + p.offset_seconds;
}
}  // namespace

double project_seconds_for_frame(const project::ProjectData& d, const project::PictureVersion& p, std::int64_t frame)
{
    return picture_origin(d, p) + frames_to_seconds(frame, p.rate);
}

double picture_seconds_at(const project::ProjectData& d, const project::PictureVersion& p, double project_seconds)
{
    return project_seconds - picture_origin(d, p);
}

std::int64_t frame_at(const project::ProjectData& d, const project::PictureVersion& p, double project_seconds)
{
    return seconds_to_frames(picture_seconds_at(d, p, project_seconds), p.rate);
}

bool rate_mismatch(const project::ProjectData& d, const project::PictureVersion& p)
{
    return std::abs(frames_per_second(d.frame_rate) - frames_per_second(p.rate)) > 1e-6 ||
           is_drop_frame(d.frame_rate) != is_drop_frame(p.rate);
}

MusicalPosition locate_in_music(const project::ProjectData& d, double seconds)
{
    MusicalPosition m;
    m.tick = d.tempo.tick_at_seconds(std::max(0.0, seconds));
    m.bar = d.meter.bar_at(m.tick);
    const TimeSignature sig = d.meter.at_bar(m.bar);
    const Ticks bar_start = d.meter.bar_start(m.bar);
    m.beat = 1.0 + static_cast<double>(m.tick - bar_start) / static_cast<double>(sig.beat_ticks());
    const Ticks beat = sig.beat_ticks();
    const Ticks k = (m.tick - bar_start + beat / 2) / beat;
    m.nearest_beat_tick = bar_start + k * beat;
    m.offset_ms = (seconds - d.tempo.seconds_at(m.nearest_beat_tick)) * 1000.0;
    m.offset_frames = m.offset_ms / 1000.0 * frames_per_second(d.frame_rate);
    return m;
}

std::optional<TempoMap> fit_tempo_to_hit(const TempoMap& tempo, Ticks from_tick, Ticks target_tick,
                                         double target_seconds, std::string* error)
{
    auto fail = [&](const std::string& why) -> std::optional<TempoMap> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (target_tick <= from_tick) return fail("the beat to place must come after the point the tempo is changed from");
    const double from_seconds = tempo.seconds_at(from_tick);
    if (target_seconds <= from_seconds) return fail("the hit is before the point the tempo is changed from");
    const double quarters = static_cast<double>(target_tick - from_tick) / static_cast<double>(kTicksPerQuarter);
    const double bpm = quarters * 60.0 / (target_seconds - from_seconds);
    if (bpm < 20.0 || bpm > 400.0)
        return fail("landing that beat on the hit needs " + std::to_string(static_cast<int>(std::lround(bpm))) +
                    " bpm; choose a beat nearer or further away");
    const double after = tempo.bpm_at(target_tick);
    TempoMap out(tempo.changes().front().bpm);
    for (const auto& c : tempo.changes())
        if (c.tick < from_tick || c.tick > target_tick) out.set(c.tick, c.bpm);
    out.set(from_tick, bpm);
    // Changes strictly inside the range are replaced by the fitted tempo;
    // the tempo that was in force after the hit carries on from it.
    bool has_change_at_target = false;
    for (const auto& c : tempo.changes()) has_change_at_target = has_change_at_target || c.tick == target_tick;
    if (!has_change_at_target) out.set(target_tick, after);
    return out;
}

std::optional<double> streamer_progress(const project::Marker& m, double now)
{
    if (m.streamer_seconds <= 0.0) return std::nullopt;
    const double start = m.seconds - m.streamer_seconds;
    if (now < start || now >= m.seconds) return std::nullopt;
    return (now - start) / m.streamer_seconds;
}

bool punch_visible(const project::Marker& m, double now, double punch_seconds)
{
    return now >= m.seconds && now < m.seconds + punch_seconds;
}

std::vector<project::FlaggedRegion> changed_regions(const std::vector<std::uint64_t>& a,
                                                    const std::vector<std::uint64_t>& b, double step,
                                                    double start, int bits)
{
    std::vector<project::FlaggedRegion> out;
    const std::size_t n = std::max(a.size(), b.size());
    std::size_t i = 0;
    while (i < n) {
        auto differs = [&](std::size_t k) {
            if (k >= a.size() || k >= b.size()) return true;  // one cut is longer
            return static_cast<int>(std::bitset<64>(a[k] ^ b[k]).count()) > bits;
        };
        if (!differs(i)) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < n && differs(j)) ++j;
        const double s = start + static_cast<double>(i) * step;
        const double e = start + static_cast<double>(j) * step;
        // Merge with the previous region when only one step apart.
        if (!out.empty() && s - out.back().end_seconds <= step + 1e-9) out.back().end_seconds = e;
        else out.push_back({s, e, j >= std::min(a.size(), b.size()) ? "length differs" : "picture changed"});
        i = j;
    }
    return out;
}

}  // namespace vcc::picture
