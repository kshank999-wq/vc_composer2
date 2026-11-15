// External jog/shuttle and transport controllers (Add-On Features spec §7,
// Feature E): what a control surface's messages mean, and where a jog or a
// shuttle takes the playhead. JUCE-free and clock-free, so every rule is
// unit-tested: the desktop feeds it raw MIDI and the time, and applies the
// positions it returns with a stepped, rate-limited locate.
#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vcc/project/Project.h"

namespace vcc::project::surface {

/// What a control does.
enum class Action {
    Jog,            ///< Relative wheel: steps the playhead.
    Shuttle,        ///< Spring-loaded ring: continuous speed and direction.
    PlayPause,
    Play,
    Stop,
    NextMarker,
    PreviousMarker,
    ReturnToStart,
    ToggleLoop,
};
const char* to_string(Action a) noexcept;
std::optional<Action> action_from_string(const std::string& text) noexcept;
/// "Jog wheel", "Play / Pause" ...
const char* display_name(Action a) noexcept;
const std::vector<Action>& all_actions();

/// How a relative (endless) control encodes its steps.
enum class Encoding {
    TwosComplement,  ///< 1..63 forward, 127..65 back (-1..-63).
    BinaryOffset,    ///< 64 is rest: 65 = +1, 63 = -1.
    SignMagnitude,   ///< 1..63 forward, 65..127 back (Mackie Control).
    Absolute,        ///< 0..127 (shuttle: 64 is the centre) or pitch bend.
};
const char* to_string(Encoding e) noexcept;
std::optional<Encoding> encoding_from_string(const std::string& text) noexcept;

/// The MIDI message a control sends.
struct Source {
    enum class Kind { Controller, Note, PitchBend };
    Kind kind = Kind::Controller;
    int channel = 0;  ///< 1..16, 0 for any.
    int number = 0;   ///< Controller or note number (unused for pitch bend).
    bool operator==(const Source& o) const noexcept
    {
        return kind == o.kind && channel == o.channel && number == o.number;
    }
};
/// "CC 60 ch 1", "Note 94", "Pitch bend ch 2".
std::string describe(const Source& s);

struct Binding {
    Action action = Action::Jog;
    Source source;
    Encoding encoding = Encoding::TwosComplement;  ///< Jog and shuttle only.
};

/// A named set of bindings: built in for common layouts, or the user's own.
struct Profile {
    std::string name;
    std::vector<Binding> bindings;
    const Binding* find(Action a) const noexcept;
};
/// "Mackie Control (jog CC 60)", "Generic MIDI (CC 16-22)", "Custom".
const std::vector<Profile>& builtin_profiles();

/// What a message did.
struct Event {
    Action action = Action::Jog;
    int steps = 0;        ///< Jog: detents, signed.
    double value = 0.0;   ///< Shuttle: -1..+1 (0 is rest).
    bool pressed = false; ///< Buttons: true on press (releases are ignored).
};

/// Decodes one raw MIDI message against a profile. Releases, unmapped
/// messages and a relative control at rest give nothing.
std::optional<Event> decode(const Profile& p, std::uint8_t status, std::uint8_t data1, std::uint8_t data2);
/// The source a raw message comes from (for learning), if it can be bound.
std::optional<Source> source_of(std::uint8_t status, std::uint8_t data1, std::uint8_t data2);
/// Whether `profile` claims this message: a surface on a shared device
/// takes only its own messages and leaves the rest to play.
bool claims(const Profile& p, std::uint8_t status, std::uint8_t data1, std::uint8_t data2);

/// What one jog detent moves.
enum class Step { Frame, Sixteenth, Beat, Bar };
const char* to_string(Step s) noexcept;
std::optional<Step> step_from_string(const std::string& text) noexcept;

enum class Curve { Linear, Exponential };

struct Settings {
    Step step = Step::Beat;
    int detents_per_step = 1;     ///< Sensitivity: 1 (every detent) .. 8.
    bool reverse = false;         ///< Both jog and shuttle.
    bool acceleration = true;     ///< Fast spins move further (up to 4x).
    double dead_zone = 0.08;      ///< Shuttle values within this of rest are rest.
    Curve curve = Curve::Exponential;
    double max_speed = 8.0;       ///< Shuttle at full turn, times real time.
    /// Audio scrub: the shuttle forward up to 4x plays at its speed (tape
    /// style), the jog and the reverse shuttle play short grains. Off, or
    /// when the engine cannot (recording, armed, automation being
    /// written), it is a silent stepped seek.
    bool audible = true;
};

/// Where things are, in project seconds: the jog's grid and the markers.
struct Timeline {
    const TempoMap* tempo = nullptr;
    const MeterMap* meter = nullptr;
    double frames_per_second = 24.0;
    double end_seconds = 0.0;      ///< The shuttle stops here (and at 0).
    std::vector<double> markers;   ///< Sorted: markers, cue starts.
};
/// The timeline of a project: its tempo, meter, picture rate, length
/// (last event, cue or marker, plus a bar of room), markers and cue starts.
Timeline timeline_of(const ProjectData& d);

/// The next or previous locate point strictly past `from` (a little
/// tolerance, so a marker the playhead sits on is skipped).
std::optional<double> next_marker(const Timeline& t, double from);
std::optional<double> previous_marker(const Timeline& t, double from);

/// Jog and shuttle state. The caller passes the time (seconds, any
/// monotonic clock) so acceleration and shuttle travel are testable.
class JogShuttle {
public:
    Settings settings;

    /// Where `detents` of the jog take a playhead at `from` seconds. Steps
    /// snap to the grid: from between two beats, one step forward lands on
    /// the next beat, not a beat's length further on. Never before 0.
    double jog(double from, int detents, double now, const Timeline& t);
    /// The detents not yet worth a step (sensitivity above 1).
    int pending_detents() const noexcept { return pending_; }

    /// The shuttle ring's raw position, -1..+1.
    void set_shuttle(double raw) noexcept;
    double shuttle_raw() const noexcept { return raw_; }
    /// Times real time, signed; 0 inside the dead zone.
    double shuttle_speed() const noexcept;
    bool shuttling() const noexcept { return std::abs(shuttle_speed()) > 0.0; }
    /// Where the shuttle has taken a playhead at `from` after `dt` seconds:
    /// within 0..end, and never more than 0.25 s of travel per call (a
    /// stalled clock cannot make it leap).
    double advance(double from, double dt, const Timeline& t) const;
    /// Back to rest (stop, unplug, a transport button).
    void reset() noexcept;

private:
    int pending_ = 0;
    double raw_ = 0.0;
    double last_jog_ = -1.0;
    double rate_ = 0.0;  ///< Detents per second, smoothed.
};

}  // namespace vcc::project::surface
