#include "vcc/picture/Picture.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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

std::string feet_frames(double seconds, FrameRate rate, int per_foot)
{
    per_foot = std::max(1, per_foot);
    const auto frames = static_cast<std::int64_t>(std::floor(std::abs(seconds) * frames_per_second(rate) + 1e-6));
    char text[48];
    std::snprintf(text, sizeof text, "%s%lld+%02lld", seconds < 0.0 && frames > 0 ? "-" : "",
                  static_cast<long long>(frames / per_foot), static_cast<long long>(frames % per_foot));
    return text;
}

std::optional<double> feet_frames_seconds(const std::string& text, FrameRate rate, int per_foot)
{
    if (text.empty() || per_foot < 1) return std::nullopt;
    std::size_t i = 0;
    const bool negative = text[0] == '-';
    if (negative) ++i;
    auto number = [&](long long& out) {
        const std::size_t from = i;
        out = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') out = out * 10 + (text[i++] - '0');
        return i > from && i - from <= 9;
    };
    long long feet = 0, frames = 0;
    if (!number(feet)) return std::nullopt;
    if (i < text.size()) {
        if (text[i] != '+') return std::nullopt;
        ++i;
        if (!number(frames) || frames >= per_foot) return std::nullopt;
    }
    if (i != text.size()) return std::nullopt;
    const double s = static_cast<double>(feet * per_foot + frames) / frames_per_second(rate);
    return negative ? -s : s;
}

std::vector<TempoCandidate> find_tempos(double start, const std::vector<double>& hits, const TempoSearch& search)
{
    std::vector<TempoCandidate> out;
    const double lo = std::max(1.0, std::min(search.min_bpm, search.max_bpm));
    const double hi = std::max(search.min_bpm, search.max_bpm);
    const double fps = search.fps > 0.0 ? search.fps : 24.0;
    const double tol = std::max(0.01, search.tolerance_frames);
    const double grid_quarters = static_cast<double>(std::max<Ticks>(1, search.grid)) / kTicksPerQuarter;
    double reach = 0.0;
    for (const double h : hits) reach = std::max(reach, h - start);
    if (reach <= 0.0) return out;

    // Every hit, at one tempo: on or not, and its error.
    auto evaluate = [&](double bpm) {
        TempoCandidate c;
        c.bpm = bpm;
        const double g = 60.0 / bpm * grid_quarters;
        double sse = 0.0;
        for (const double h : hits) {
            const double dt = h - start;
            if (dt <= 0.0) {
                c.error_frames.push_back(0.0);
                c.hit_ticks.push_back(-1);
                continue;
            }
            const double n = std::round(dt / g);
            const double e = (dt - n * g) * fps;
            c.error_frames.push_back(e);
            c.hit_ticks.push_back(static_cast<Ticks>(n) * std::max<Ticks>(1, search.grid));
            if (std::abs(e) <= tol + 1e-9) {
                ++c.hits_on;
                c.worst_frames = std::max(c.worst_frames, std::abs(e));
                sse += e * e;
            }
        }
        return std::make_pair(c, sse);
    };
    // A step small enough that the farthest hit moves a quarter of the
    // tolerance from one tempo to the next.
    const double step = std::max(0.0005, tol * lo / (4.0 * reach * fps));
    struct Scored {
        double bpm;
        int on;
        double sse;
    };
    std::vector<Scored> scored;
    for (double bpm = lo; bpm <= hi + 1e-9; bpm += step) {
        const auto e = evaluate(bpm);
        if (e.first.hits_on > 0) scored.push_back({bpm, e.first.hits_on, e.second});
    }
    std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        return a.on != b.on ? a.on > b.on : a.sse < b.sse;
    });
    for (const auto& s : scored) {
        if (out.size() >= std::max<std::size_t>(1, search.max_results)) break;
        // Least squares over the hits on: the grid length that puts them
        // nearest their beats.
        TempoCandidate c = evaluate(s.bpm).first;
        double num = 0.0, den = 0.0;
        const double g = 60.0 / s.bpm * grid_quarters;
        for (std::size_t i = 0; i < hits.size(); ++i) {
            const double dt = hits[i] - start;
            if (dt <= 0.0 || std::abs(c.error_frames[i]) > tol + 1e-9) continue;
            const double n = std::round(dt / g);
            num += n * dt;
            den += n * n;
        }
        if (den > 0.0) {
            const double fitted = 60.0 * grid_quarters / (num / den);
            if (fitted >= lo - 1e-9 && fitted <= hi + 1e-9) {
                TempoCandidate better = evaluate(fitted).first;
                if (better.hits_on >= c.hits_on) c = std::move(better);
            }
        }
        const bool seen = std::any_of(out.begin(), out.end(), [&](const TempoCandidate& o) {
            return std::abs(o.bpm - c.bpm) < 0.1;
        });
        if (!seen) out.push_back(std::move(c));
    }
    return out;
}

}  // namespace vcc::picture
