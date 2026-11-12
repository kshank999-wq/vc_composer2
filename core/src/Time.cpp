#include "vcc/core/Time.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace vcc {

// --- TimeSignature ----------------------------------------------------------

bool TimeSignature::is_valid() const noexcept
{
    if (numerator < 1 || numerator > 64) return false;
    switch (denominator) {
        case 1: case 2: case 4: case 8: case 16: case 32: return true;
        default: return false;
    }
}

std::vector<int> TimeSignature::default_grouping() const
{
    if (is_compound()) return std::vector<int>(static_cast<std::size_t>(numerator / 3), 3);
    if (denominator >= 8 && numerator > 3) {
        // Irregular eighth meters: twos first, the three last (7/8 = 2+2+3,
        // 5/8 = 2+3, 11/8 = 2+2+2+2+3). A composer can override per bar.
        std::vector<int> g;
        int remaining = numerator;
        while (remaining > 3) {
            if (remaining == 5) { g.push_back(2); g.push_back(3); return g; }
            g.push_back(2);
            remaining -= 2;
        }
        g.push_back(remaining);
        return g;
    }
    if (denominator <= 4 && numerator == 5) return {3, 2};
    if (denominator <= 4 && numerator == 7) return {2, 2, 3};
    return std::vector<int>(static_cast<std::size_t>(numerator), 1);
}

std::string TimeSignature::to_string() const
{
    return std::to_string(numerator) + "/" + std::to_string(denominator);
}

std::optional<TimeSignature> TimeSignature::parse(const std::string& text)
{
    auto slash = text.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= text.size()) return std::nullopt;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (i != slash && !(text[i] >= '0' && text[i] <= '9')) return std::nullopt;
    if (slash > 3 || text.size() - slash - 1 > 3) return std::nullopt;
    TimeSignature sig{std::atoi(text.substr(0, slash).c_str()), std::atoi(text.substr(slash + 1).c_str())};
    if (!sig.is_valid()) return std::nullopt;
    return sig;
}

bool operator==(const TimeSignature& a, const TimeSignature& b) noexcept
{
    return a.numerator == b.numerator && a.denominator == b.denominator;
}
bool operator!=(const TimeSignature& a, const TimeSignature& b) noexcept { return !(a == b); }

// --- MeterMap ---------------------------------------------------------------

MeterMap::MeterMap() : MeterMap(TimeSignature{}) {}
MeterMap::MeterMap(TimeSignature initial) { changes_.push_back({1, initial}); }

void MeterMap::set(int bar, TimeSignature sig)
{
    if (bar < 1 || !sig.is_valid()) return;
    auto it = std::lower_bound(changes_.begin(), changes_.end(), bar,
                               [](const Change& c, int b) { return c.bar < b; });
    if (it != changes_.end() && it->bar == bar) it->signature = sig;
    else changes_.insert(it, Change{bar, sig});
}

TimeSignature MeterMap::at_bar(int bar) const noexcept
{
    TimeSignature sig = changes_.front().signature;
    for (const auto& c : changes_) {
        if (c.bar > bar) break;
        sig = c.signature;
    }
    return sig;
}

Ticks MeterMap::bar_start(int bar) const noexcept
{
    if (bar <= 1) return 0;
    Ticks tick = 0;
    for (std::size_t i = 0; i < changes_.size(); ++i) {
        int seg_start = changes_[i].bar;
        int seg_end = (i + 1 < changes_.size()) ? changes_[i + 1].bar : bar;
        int last = std::min(seg_end, bar);
        if (last > seg_start) tick += static_cast<Ticks>(last - seg_start) * changes_[i].signature.ticks_per_bar();
        if (seg_end >= bar) break;
    }
    return tick;
}

int MeterMap::bar_at(Ticks tick) const noexcept
{
    if (tick <= 0) return 1;
    Ticks seg_tick = 0;
    for (std::size_t i = 0; i < changes_.size(); ++i) {
        Ticks per_bar = changes_[i].signature.ticks_per_bar();
        bool last = i + 1 == changes_.size();
        Ticks seg_len = last ? 0 : static_cast<Ticks>(changes_[i + 1].bar - changes_[i].bar) * per_bar;
        if (last || tick < seg_tick + seg_len)
            return changes_[i].bar + static_cast<int>((tick - seg_tick) / per_bar);
        seg_tick += seg_len;
    }
    return 1;
}

TimeSignature MeterMap::at_tick(Ticks tick) const noexcept { return at_bar(bar_at(tick)); }

Ticks MeterMap::span(int first_bar, int count) const noexcept
{
    return bar_start(first_bar + count) - bar_start(first_bar);
}

// --- TempoMap ---------------------------------------------------------------

TempoMap::TempoMap() : TempoMap(120.0) {}
TempoMap::TempoMap(double initial_bpm) { changes_.push_back({0, initial_bpm > 0 ? initial_bpm : 120.0}); }

void TempoMap::set(Ticks tick, double bpm)
{
    if (bpm <= 0.0 || tick < 0) return;
    auto it = std::lower_bound(changes_.begin(), changes_.end(), tick,
                               [](const Change& c, Ticks t) { return c.tick < t; });
    if (it != changes_.end() && it->tick == tick) it->bpm = bpm;
    else changes_.insert(it, Change{tick, bpm});
}

double TempoMap::bpm_at(Ticks tick) const noexcept
{
    double bpm = changes_.front().bpm;
    for (const auto& c : changes_) {
        if (c.tick > tick) break;
        bpm = c.bpm;
    }
    return bpm;
}

double TempoMap::seconds_at(Ticks tick) const noexcept
{
    double seconds = 0.0;
    for (std::size_t i = 0; i < changes_.size(); ++i) {
        Ticks start = changes_[i].tick;
        Ticks end = (i + 1 < changes_.size()) ? changes_[i + 1].tick : tick;
        Ticks stop = std::min(end, tick);
        double sec_per_tick = 60.0 / (changes_[i].bpm * static_cast<double>(kTicksPerQuarter));
        if (stop > start) seconds += static_cast<double>(stop - start) * sec_per_tick;
        if (end >= tick) break;
    }
    if (tick < 0) seconds = static_cast<double>(tick) * 60.0 / (changes_.front().bpm * kTicksPerQuarter);
    return seconds;
}

Ticks TempoMap::tick_at_seconds(double seconds) const noexcept
{
    if (seconds <= 0) return 0;
    double elapsed = 0.0;
    for (std::size_t i = 0; i < changes_.size(); ++i) {
        double sec_per_tick = 60.0 / (changes_[i].bpm * static_cast<double>(kTicksPerQuarter));
        bool last = i + 1 == changes_.size();
        double seg = last ? 0.0 : static_cast<double>(changes_[i + 1].tick - changes_[i].tick) * sec_per_tick;
        if (last || seconds < elapsed + seg)
            return changes_[i].tick + static_cast<Ticks>(std::llround((seconds - elapsed) / sec_per_tick));
        elapsed += seg;
    }
    return 0;
}

// --- Frame rates and timecode -------------------------------------------------

const char* to_string(FrameRate rate) noexcept
{
    switch (rate) {
        case FrameRate::Fps23_976: return "23.976";
        case FrameRate::Fps24: return "24";
        case FrameRate::Fps25: return "25";
        case FrameRate::Fps29_97_NonDrop: return "29.97";
        case FrameRate::Fps29_97_Drop: return "29.97df";
        case FrameRate::Fps30: return "30";
        case FrameRate::Fps47_952: return "47.952";
        case FrameRate::Fps48: return "48";
        case FrameRate::Fps50: return "50";
        case FrameRate::Fps59_94_NonDrop: return "59.94";
        case FrameRate::Fps59_94_Drop: return "59.94df";
        case FrameRate::Fps60: return "60";
    }
    return "24";
}

std::optional<FrameRate> frame_rate_from_string(const std::string& text) noexcept
{
    static const FrameRate all[] = {
        FrameRate::Fps23_976, FrameRate::Fps24, FrameRate::Fps25, FrameRate::Fps29_97_NonDrop,
        FrameRate::Fps29_97_Drop, FrameRate::Fps30, FrameRate::Fps47_952, FrameRate::Fps48,
        FrameRate::Fps50, FrameRate::Fps59_94_NonDrop, FrameRate::Fps59_94_Drop, FrameRate::Fps60};
    for (FrameRate r : all)
        if (text == to_string(r)) return r;
    return std::nullopt;
}

double frames_per_second(FrameRate rate) noexcept
{
    switch (rate) {
        case FrameRate::Fps23_976: return 24000.0 / 1001.0;
        case FrameRate::Fps24: return 24.0;
        case FrameRate::Fps25: return 25.0;
        case FrameRate::Fps29_97_NonDrop:
        case FrameRate::Fps29_97_Drop: return 30000.0 / 1001.0;
        case FrameRate::Fps30: return 30.0;
        case FrameRate::Fps47_952: return 48000.0 / 1001.0;
        case FrameRate::Fps48: return 48.0;
        case FrameRate::Fps50: return 50.0;
        case FrameRate::Fps59_94_NonDrop:
        case FrameRate::Fps59_94_Drop: return 60000.0 / 1001.0;
        case FrameRate::Fps60: return 60.0;
    }
    return 24.0;
}

int nominal_fps(FrameRate rate) noexcept
{
    return static_cast<int>(std::lround(frames_per_second(rate)));
}

bool is_drop_frame(FrameRate rate) noexcept
{
    return rate == FrameRate::Fps29_97_Drop || rate == FrameRate::Fps59_94_Drop;
}

namespace {
/// Frame numbers skipped at the start of each non-tenth minute: 2 at 29.97, 4 at 59.94.
std::int64_t drop_count(FrameRate rate) noexcept { return rate == FrameRate::Fps59_94_Drop ? 4 : 2; }
}  // namespace

Timecode frames_to_timecode(std::int64_t frame_count, FrameRate rate) noexcept
{
    Timecode tc;
    if (frame_count < 0) {
        tc = frames_to_timecode(-frame_count, rate);
        tc.negative = true;
        return tc;
    }
    const std::int64_t fps = nominal_fps(rate);
    std::int64_t f = frame_count;
    if (is_drop_frame(rate)) {
        const std::int64_t d = drop_count(rate);
        const std::int64_t per_min = fps * 60 - d;           // frames in a dropping minute
        const std::int64_t per_10min = fps * 600 - d * 9;    // frames in ten minutes
        std::int64_t tens = f / per_10min;
        std::int64_t rem = f % per_10min;
        // The first minute of every ten keeps all its labels.
        std::int64_t extra = rem < d ? 0 : d * ((rem - d) / per_min);
        f += d * 9 * tens + extra;
    }
    tc.frames = static_cast<int>(f % fps);
    std::int64_t total_seconds = f / fps;
    tc.seconds = static_cast<int>(total_seconds % 60);
    tc.minutes = static_cast<int>((total_seconds / 60) % 60);
    tc.hours = static_cast<int>(total_seconds / 3600);
    return tc;
}

std::optional<std::int64_t> timecode_to_frames(const Timecode& tc, FrameRate rate) noexcept
{
    const int fps = nominal_fps(rate);
    if (tc.hours < 0 || tc.minutes < 0 || tc.minutes > 59 || tc.seconds < 0 || tc.seconds > 59 ||
        tc.frames < 0 || tc.frames >= fps)
        return std::nullopt;
    std::int64_t total_minutes = static_cast<std::int64_t>(tc.hours) * 60 + tc.minutes;
    std::int64_t frames =
        ((total_minutes * 60) + tc.seconds) * fps + tc.frames;
    if (is_drop_frame(rate)) {
        const std::int64_t d = drop_count(rate);
        if (tc.seconds == 0 && tc.frames < d && tc.minutes % 10 != 0) return std::nullopt;
        frames -= d * (total_minutes - total_minutes / 10);
    }
    return tc.negative ? -frames : frames;
}

double frames_to_seconds(std::int64_t frame_count, FrameRate rate) noexcept
{
    return static_cast<double>(frame_count) / frames_per_second(rate);
}

std::int64_t seconds_to_frames(double seconds, FrameRate rate) noexcept
{
    // A tiny epsilon keeps an exact frame boundary from flooring to the frame
    // before it through floating-point error.
    return static_cast<std::int64_t>(std::floor(seconds * frames_per_second(rate) + 1e-9));
}

std::string Timecode::to_string(FrameRate rate) const
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s%02d:%02d:%02d%c%02d", negative ? "-" : "", hours, minutes,
                  seconds, is_drop_frame(rate) ? ';' : ':', frames);
    return buf;
}

std::optional<Timecode> Timecode::parse(const std::string& text)
{
    Timecode tc;
    std::string s = text;
    if (!s.empty() && s[0] == '-') {
        tc.negative = true;
        s = s.substr(1);
    }
    int parts[4] = {0, 0, 0, 0};
    int idx = 0;
    std::string cur;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            cur += c;
        } else if (c == ':' || c == ';' || c == '.') {
            if (cur.empty() || idx >= 3) return std::nullopt;
            parts[idx++] = std::atoi(cur.c_str());
            cur.clear();
        } else {
            return std::nullopt;
        }
    }
    if (cur.empty() || idx != 3) return std::nullopt;
    parts[3] = std::atoi(cur.c_str());
    tc.hours = parts[0];
    tc.minutes = parts[1];
    tc.seconds = parts[2];
    tc.frames = parts[3];
    return tc;
}

}  // namespace vcc
