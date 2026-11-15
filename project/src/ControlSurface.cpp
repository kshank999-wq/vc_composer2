#include "vcc/project/ControlSurface.h"

#include <algorithm>
#include <cmath>

namespace vcc::project::surface {

namespace {

struct ActionName {
    Action action;
    const char* id;
    const char* display;
};
const ActionName kActions[] = {
    {Action::Jog, "jog", "Jog wheel"},
    {Action::Shuttle, "shuttle", "Shuttle ring"},
    {Action::PlayPause, "play_pause", "Play / Pause"},
    {Action::Play, "play", "Play"},
    {Action::Stop, "stop", "Stop"},
    {Action::NextMarker, "next_marker", "Next marker"},
    {Action::PreviousMarker, "previous_marker", "Previous marker"},
    {Action::ReturnToStart, "return_to_start", "Return to start"},
    {Action::ToggleLoop, "toggle_loop", "Loop on / off"},
};

int channel_of(std::uint8_t status) { return (status & 0x0F) + 1; }

bool matches(const Source& s, std::uint8_t status, std::uint8_t data1)
{
    const int type = status & 0xF0;
    if (s.channel != 0 && s.channel != channel_of(status)) return false;
    switch (s.kind) {
        case Source::Kind::Controller: return type == 0xB0 && s.number == data1;
        case Source::Kind::Note: return (type == 0x90 || type == 0x80) && s.number == data1;
        case Source::Kind::PitchBend: return type == 0xE0;
    }
    return false;
}

int relative_steps(Encoding e, int v)
{
    switch (e) {
        case Encoding::TwosComplement: return v < 64 ? v : v - 128;
        case Encoding::BinaryOffset: return v - 64;
        case Encoding::SignMagnitude: return v < 64 ? v : -(v - 64);
        case Encoding::Absolute: return 0;
    }
    return 0;
}

constexpr double kMarkerTolerance = 0.002;  // seconds
constexpr Ticks kGridTolerance = 2;          // ticks

/// The grid line `n` steps from `tick` (n > 0 forward, n < 0 back), never
/// counting the line the playhead is on.
Ticks grid_step(const MeterMap& meter, Ticks tick, Step step, int n)
{
    auto line_after = [&](Ticks t) {
        const int bar = meter.bar_at(t);
        const Ticks start = meter.bar_start(bar);
        const Ticks next_bar = meter.bar_start(bar + 1);
        if (step == Step::Bar) return next_bar;
        const Ticks unit = step == Step::Beat ? meter.at_bar(bar).beat_ticks() : kTicksPerQuarter / 4;
        const Ticks k = (t - start + kGridTolerance) / unit + 1;
        return std::min(next_bar, start + k * unit);
    };
    auto line_before = [&](Ticks t) -> Ticks {
        if (t <= kGridTolerance) return 0;
        int bar = meter.bar_at(t);
        Ticks start = meter.bar_start(bar);
        if (t - start <= kGridTolerance) {
            // On a bar line: back into the previous bar.
            if (bar <= 1) return 0;
            --bar;
            start = meter.bar_start(bar);
            if (step == Step::Bar) return start;
            const Ticks unit = step == Step::Beat ? meter.at_bar(bar).beat_ticks() : kTicksPerQuarter / 4;
            const Ticks length = meter.bar_start(bar + 1) - start;
            return start + ((length - 1) / unit) * unit;
        }
        if (step == Step::Bar) return start;
        const Ticks unit = step == Step::Beat ? meter.at_bar(bar).beat_ticks() : kTicksPerQuarter / 4;
        const Ticks k = (t - start - kGridTolerance - 1) / unit;
        return start + std::max<Ticks>(0, k) * unit;
    };
    for (; n > 0; --n) tick = line_after(tick);
    for (; n < 0; ++n) tick = line_before(tick);
    return std::max<Ticks>(0, tick);
}

}  // namespace

const char* to_string(Action a) noexcept
{
    for (const auto& n : kActions)
        if (n.action == a) return n.id;
    return "jog";
}

std::optional<Action> action_from_string(const std::string& text) noexcept
{
    for (const auto& n : kActions)
        if (text == n.id) return n.action;
    return std::nullopt;
}

const char* display_name(Action a) noexcept
{
    for (const auto& n : kActions)
        if (n.action == a) return n.display;
    return "";
}

const std::vector<Action>& all_actions()
{
    static const std::vector<Action> all = [] {
        std::vector<Action> v;
        for (const auto& n : kActions) v.push_back(n.action);
        return v;
    }();
    return all;
}

const char* to_string(Encoding e) noexcept
{
    switch (e) {
        case Encoding::TwosComplement: return "twos_complement";
        case Encoding::BinaryOffset: return "binary_offset";
        case Encoding::SignMagnitude: return "sign_magnitude";
        case Encoding::Absolute: return "absolute";
    }
    return "twos_complement";
}

std::optional<Encoding> encoding_from_string(const std::string& text) noexcept
{
    for (auto e : {Encoding::TwosComplement, Encoding::BinaryOffset, Encoding::SignMagnitude, Encoding::Absolute})
        if (text == to_string(e)) return e;
    return std::nullopt;
}

std::string describe(const Source& s)
{
    std::string text;
    switch (s.kind) {
        case Source::Kind::Controller: text = "CC " + std::to_string(s.number); break;
        case Source::Kind::Note: text = "Note " + std::to_string(s.number); break;
        case Source::Kind::PitchBend: text = "Pitch bend"; break;
    }
    if (s.channel != 0) text += " ch " + std::to_string(s.channel);
    return text;
}

const Binding* Profile::find(Action a) const noexcept
{
    for (const auto& b : bindings)
        if (b.action == a) return &b;
    return nullptr;
}

const std::vector<Profile>& builtin_profiles()
{
    static const std::vector<Profile> profiles = [] {
        using K = Source::Kind;
        auto cc = [](Action a, int n, Encoding e = Encoding::Absolute) { return Binding{a, {K::Controller, 0, n}, e}; };
        auto note = [](Action a, int n) { return Binding{a, {K::Note, 0, n}, Encoding::Absolute}; };
        std::vector<Profile> v;
        // Mackie Control's jog wheel and transport keys, as most DAW
        // controllers send them in "Mackie" mode.
        v.push_back({"Mackie Control (jog CC 60)",
                     {cc(Action::Jog, 60, Encoding::SignMagnitude), note(Action::Play, 94), note(Action::Stop, 93),
                      note(Action::PreviousMarker, 91), note(Action::NextMarker, 92), note(Action::ToggleLoop, 86)}});
        // A plain layout for programmable controllers (and the checks).
        v.push_back({"Generic MIDI (CC 16-23)",
                     {cc(Action::Jog, 16, Encoding::TwosComplement), cc(Action::Shuttle, 17), cc(Action::PlayPause, 18),
                      cc(Action::Stop, 19), cc(Action::PreviousMarker, 20), cc(Action::NextMarker, 21),
                      cc(Action::ReturnToStart, 22), cc(Action::ToggleLoop, 23)}});
        v.push_back({"Custom", {}});
        return v;
    }();
    return profiles;
}

std::optional<Source> source_of(std::uint8_t status, std::uint8_t data1, std::uint8_t data2)
{
    const int type = status & 0xF0;
    const int ch = channel_of(status);
    if (type == 0xB0) return Source{Source::Kind::Controller, ch, data1};
    if (type == 0x90 && data2 > 0) return Source{Source::Kind::Note, ch, data1};
    if (type == 0xE0) return Source{Source::Kind::PitchBend, ch, 0};
    return std::nullopt;
}

bool claims(const Profile& p, std::uint8_t status, std::uint8_t data1, std::uint8_t)
{
    for (const auto& b : p.bindings)
        if (matches(b.source, status, data1)) return true;
    return false;
}

std::optional<Event> decode(const Profile& p, std::uint8_t status, std::uint8_t data1, std::uint8_t data2)
{
    const int type = status & 0xF0;
    for (const auto& b : p.bindings) {
        if (!matches(b.source, status, data1)) continue;
        Event e;
        e.action = b.action;
        if (b.action == Action::Jog) {
            if (type != 0xB0) return std::nullopt;
            e.steps = relative_steps(b.encoding, data2 & 0x7F);
            if (e.steps == 0) return std::nullopt;
            return e;
        }
        if (b.action == Action::Shuttle) {
            if (type == 0xE0) {
                const int v = (data2 << 7) | data1;
                e.value = std::clamp((v - 8192) / 8191.0, -1.0, 1.0);
            } else if (type == 0xB0) {
                e.value = std::clamp((data2 - 64) / 63.0, -1.0, 1.0);
            } else {
                return std::nullopt;
            }
            return e;
        }
        // A button: presses only.
        const bool press = (type == 0x90 && data2 > 0) || (type == 0xB0 && data2 > 0);
        if (!press) return std::nullopt;
        e.pressed = true;
        return e;
    }
    return std::nullopt;
}

const char* to_string(Step s) noexcept
{
    switch (s) {
        case Step::Frame: return "frame";
        case Step::Sixteenth: return "sixteenth";
        case Step::Beat: return "beat";
        case Step::Bar: return "bar";
    }
    return "beat";
}

std::optional<Step> step_from_string(const std::string& text) noexcept
{
    for (auto s : {Step::Frame, Step::Sixteenth, Step::Beat, Step::Bar})
        if (text == to_string(s)) return s;
    return std::nullopt;
}

Timeline timeline_of(const ProjectData& d)
{
    Timeline t;
    t.tempo = &d.tempo;
    t.meter = &d.meter;
    const auto* picture = d.active_picture();
    t.frames_per_second = frames_per_second(picture ? picture->rate : d.frame_rate);
    Ticks end = d.meter.bar_start(33);
    double audio_end = 0.0;
    for (const auto& track : d.tracks) {
        const auto* v = track.active_version();
        if (!v) continue;
        end = std::max(end, v->score.end_tick());
        for (const auto& r : v->regions) end = std::max(end, r.end);
        for (const auto& c : v->audio_clips) audio_end = std::max(audio_end, c.start_seconds + c.length);
    }
    for (const auto& c : d.cues) {
        end = std::max(end, c.end);
        t.markers.push_back(d.tempo.seconds_at(c.start));
    }
    for (const auto& m : d.markers) {
        t.markers.push_back(m.seconds);
        audio_end = std::max(audio_end, m.seconds);
    }
    t.end_seconds = std::max(d.tempo.seconds_at(d.meter.bar_start(d.meter.bar_at(end - 1) + 2)), audio_end + 2.0);
    std::sort(t.markers.begin(), t.markers.end());
    t.markers.erase(std::unique(t.markers.begin(), t.markers.end(),
                                [](double a, double b) { return std::abs(a - b) < kMarkerTolerance; }),
                    t.markers.end());
    return t;
}

std::optional<double> next_marker(const Timeline& t, double from)
{
    for (double m : t.markers)
        if (m > from + kMarkerTolerance) return m;
    return std::nullopt;
}

std::optional<double> previous_marker(const Timeline& t, double from)
{
    for (auto it = t.markers.rbegin(); it != t.markers.rend(); ++it)
        if (*it < from - kMarkerTolerance) return *it;
    return std::nullopt;
}

double JogShuttle::jog(double from, int detents, double now, const Timeline& t)
{
    if (detents == 0) return from;
    if (settings.reverse) detents = -detents;
    // Speed of the spin, for acceleration: detents per second, smoothed.
    const double dt = last_jog_ < 0.0 ? 1.0 : now - last_jog_;
    last_jog_ = now;
    const double instant = std::abs(detents) / std::max(dt, 0.004);
    rate_ = dt > 0.4 ? instant : rate_ * 0.6 + instant * 0.4;
    // Sensitivity: detents gather until they make a step; a turn the
    // other way starts afresh.
    if ((pending_ > 0 && detents < 0) || (pending_ < 0 && detents > 0)) pending_ = 0;
    pending_ += detents;
    const int per = std::clamp(settings.detents_per_step, 1, 8);
    int steps = pending_ / per;
    pending_ -= steps * per;
    if (steps == 0) return from;
    if (settings.acceleration) {
        const int factor = rate_ < 10.0 ? 1 : rate_ < 20.0 ? 2 : rate_ < 35.0 ? 3 : 4;
        steps *= factor;
    }
    if (settings.step == Step::Frame || !t.tempo || !t.meter) {
        const double fps = t.frames_per_second > 0.0 ? t.frames_per_second : 24.0;
        // Frames: onto the frame grid, then whole frames.
        const double frame = from * fps;
        double target = steps > 0 ? std::floor(frame + 1e-6) + steps : std::ceil(frame - 1e-6) + steps;
        return std::max(0.0, target / fps);
    }
    const Ticks tick = t.tempo->tick_at_seconds(std::max(0.0, from));
    return t.tempo->seconds_at(grid_step(*t.meter, tick, settings.step, steps));
}

void JogShuttle::set_shuttle(double raw) noexcept { raw_ = std::clamp(raw, -1.0, 1.0); }

double JogShuttle::shuttle_speed() const noexcept
{
    const double r = settings.reverse ? -raw_ : raw_;
    const double a = std::abs(r);
    const double dz = std::clamp(settings.dead_zone, 0.0, 0.5);
    if (a <= dz) return 0.0;
    const double x = std::min(1.0, (a - dz) / (1.0 - dz));
    const double shaped = settings.curve == Curve::Linear ? x : (std::exp(3.0 * x) - 1.0) / (std::exp(3.0) - 1.0);
    const double top = std::clamp(settings.max_speed, 0.5, 32.0);
    const double speed = 0.1 + (top - 0.1) * shaped;
    return r < 0 ? -speed : speed;
}

double JogShuttle::advance(double from, double dt, const Timeline& t) const
{
    const double speed = shuttle_speed();
    if (speed == 0.0) return from;
    const double step = std::clamp(dt, 0.0, 0.25);
    double to = from + speed * step;
    if (t.end_seconds > 0.0) to = std::min(to, t.end_seconds);
    return std::max(0.0, to);
}

void JogShuttle::reset() noexcept
{
    raw_ = 0.0;
    pending_ = 0;
    rate_ = 0.0;
}

}  // namespace vcc::project::surface
