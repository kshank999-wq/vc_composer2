#include "audio/PlaybackEngine.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "audio/DiskStreamer.h"
#include "audio/RemotePlugin.h"
#include "audio/AutomationRt.h"
#include "audio/ChannelDsp.h"
#include "audio/PlaceholderSynth.h"
#include "audio/Recorder.h"
#include "vcc/project/Automation.h"
#include "vcc/project/Render.h"

namespace vcc::desktop {

// --- Snapshot ----------------------------------------------------------------------

namespace {

struct Event {
    std::int64_t sample = 0;
    std::uint8_t bytes[3] = {0, 0, 0};
    std::uint8_t size = 3;
    std::uint8_t order = 1;  // 0 = off/controller first at the same sample, 1 = on
};

float db_to_gain(double db) { return db <= -96.0 ? 0.0f : static_cast<float>(std::pow(10.0, db / 20.0)); }

constexpr int kMaxSends = 16;
constexpr double kDeclickSeconds = 0.003;

constexpr double kMuteFadeSeconds = 0.001;

/// Every mix setting of one strip, computed on the message thread and sent
/// to the audio thread through a Mailbox. Gains are final (fader, pan law,
/// mute/solo folded in), so the audio thread only ramps to them. The raw
/// settings travel too, so automation can override one and recompute the
/// gains on the audio thread (finalize()).
struct StripParams {
    project::ChannelStrip strip;
    float out_l = 0.0f, out_r = 0.0f;
    std::array<float, kMaxSends> send_l{}, send_r{};
    // Raw settings.
    double gain_db = 0.0, pan = 0.0;
    bool mute = false, solo_ok = true, mono = false;
    int num_sends = 0;
    std::array<double, kMaxSends> send_db{}, send_pan{};
    std::array<std::uint8_t, kMaxSends> send_flags{};  // 1 pre-fader, 2 muted, 4 pan follows
};

void store_max(std::atomic<float>& a, float v) noexcept
{
    float old = a.load(std::memory_order_relaxed);
    while (v > old && !a.compare_exchange_weak(old, v, std::memory_order_relaxed)) {}
}

void store_min(std::atomic<float>& a, float v) noexcept
{
    float old = a.load(std::memory_order_relaxed);
    while (v < old && !a.compare_exchange_weak(old, v, std::memory_order_relaxed)) {}
}

/// Constant-power pan (-3 dB centre) for a mono source; balance for stereo.
std::pair<float, float> pan_gains(double pan, bool mono)
{
    const double angle = (std::clamp(pan, -1.0, 1.0) + 1.0) * juce::MathConstants<double>::pi / 4.0;
    const double l = std::cos(angle), r = std::sin(angle);
    if (mono) return {static_cast<float>(l), static_cast<float>(r)};
    return {static_cast<float>(std::min(1.0, l * juce::MathConstants<double>::sqrt2)),
            static_cast<float>(std::min(1.0, r * juce::MathConstants<double>::sqrt2))};
}

bool is_mono_track(const project::Track& t)
{
    return (t.kind == project::TrackKind::Audio || t.kind == project::TrackKind::Aux) && !t.input_stereo;
}

bool has_instrument(const project::Track& t)
{
    return t.kind == project::TrackKind::Instrument || t.kind == project::TrackKind::MidiOnly;
}

/// Suggested inserts are inactive, except the reverb/delay a template puts
/// on its effect returns (so its sends are heard).
bool insert_active(const project::InsertSlot& slot)
{
    return is_known_insert_kind(slot.kind) && (!slot.suggestion || slot.kind == "reverb" || slot.kind == "delay" || slot.kind == "plugin");
}

}  // namespace

/// One console strip's persistent processing and meters (message-thread map,
/// shared into snapshots).
struct StripDsp {
    ChannelProcessor channel;
    Mailbox<StripParams> params;
    // Audio thread: ramp state.
    bool primed = false;
    float cur_l = 0.0f, cur_r = 0.0f;
    std::array<float, kMaxSends> cur_send_l{}, cur_send_r{};
    // Audio thread: this block's automated settings.
    StripParams eff;
    bool use_eff = false;
    int mute_mode = 0;  // 0 static, 1 lane (sample-accurate curve), 2 written value (ramped)
    const Curve* mute_curve = nullptr;
    std::int64_t mute_from = 0;
    float mute_cur = 1.0f, mute_target = 1.0f;
    // Meters (audio thread writes, message thread takes).
    std::atomic<float> peak_l{0.0f}, peak_r{0.0f}, gr_db{0.0f};
    std::atomic<bool> gate_closed{false}, clipped{false};
};

/// The value being written for one (owner, param), shared by the message
/// thread (writer) and the audio thread.
struct AutoSlot {
    std::atomic<int> mode{0};  // 0 play the lane, 1 play `value`, 2 play the static value
    std::atomic<double> value{0.0};
};

namespace {
enum class AutoKind : std::uint8_t { Volume, Pan, Mute, Send, Strip, Insert, Plugin };

/// One automated parameter of a strip, ready for the audio thread.
struct AutoParam {
    AutoKind kind = AutoKind::Volume;
    int a = -1, b = -1;  // send index / strip field / insert index + param index
    juce::AudioProcessorParameter* param = nullptr;  // plug-in parameter
    Curve curve;                                     // empty: written only
    std::shared_ptr<AutoSlot> slot;
    std::int64_t offset = 0;  // processing latency before this point of the strip
    float last_sent = -1.0f;  // audio thread
};
}  // namespace

struct PlaybackSnapshot {
    double sample_rate = 48000.0;
    int max_block = 512;
    std::int64_t comp = 0;  // delay compensation: what is heard is the render position minus this

    struct Strip {
        std::shared_ptr<StripDsp> dsp;
        std::vector<std::shared_ptr<InsertProcessor>> inserts;
        std::vector<int> sends;  // destination bus per send (-1 = none), aligned with StripParams
        int output = -1;         // bus index; -1 = the device (master bus only)
        std::vector<AutoParam> autos;
        // Delay compensation per routing edge (0 = none).
        int out_delay = 0;
        std::shared_ptr<DelayLine> out_line;
        std::array<int, kMaxSends> send_delay{};
        std::array<std::shared_ptr<DelayLine>, kMaxSends> send_lines;
    };
    struct Clip {
        std::shared_ptr<ClipStream> stream;
        std::int64_t start = 0, length = 0, offset = 0;  // timeline / file samples at sample_rate
        std::int64_t fade_in = 0, fade_out = 0, declick = 0;
        float gain = 1.0f;
    };
    struct Track {
        Strip strip;
        std::shared_ptr<Instrument> instrument;  // null for audio tracks
        std::vector<Event> events;
        std::size_t cursor = 0;  // audio thread
        std::vector<Clip> clips;
        bool mono = false;
        int monitor = 1;  // 0 off, 1 auto, 2 input
        bool armed = false;
        int input = -1;
        bool input_stereo = false;
        juce::AudioBuffer<float> buffer;
        juce::MidiBuffer midi;
    };
    struct Bus {
        Strip strip;
        juce::AudioBuffer<float> buffer;
    };
    std::vector<Track> tracks;
    std::vector<Bus> buses;
    std::vector<int> bus_order;  // sources before destinations; the master last
    std::vector<std::pair<std::int64_t, bool>> clicks;  // sample, downbeat
    std::size_t click_cursor = 0;
    juce::AudioBuffer<float> out;  // the master bus's output: what the device plays
    int live_track = -1;

    void seek(std::int64_t sample) noexcept
    {
        for (auto& t : tracks)
            t.cursor = static_cast<std::size_t>(
                std::lower_bound(t.events.begin(), t.events.end(), sample,
                                 [](const Event& e, std::int64_t s) { return e.sample < s; }) -
                t.events.begin());
        // The click is scheduled against what is heard.
        click_cursor = static_cast<std::size_t>(
            std::lower_bound(clicks.begin(), clicks.end(), sample - comp,
                             [](const std::pair<std::int64_t, bool>& c, std::int64_t s) { return c.first < s; }) -
            clicks.begin());
    }
    template <class F>
    void for_each_strip(F&& f) noexcept
    {
        for (auto& t : tracks) f(t.strip);
        for (auto& b : buses) f(b.strip);
    }
};

struct PreviewClip {
    std::unique_ptr<PlaceholderSynth> synth;
    std::vector<Event> events;
    std::int64_t length = 0;
    bool loop = false;
    std::int64_t position = 0;
    std::size_t cursor = 0;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer;
};

namespace {

std::vector<Event> to_events(const std::vector<NoteEvent>& notes, const std::vector<ControllerEvent>& controllers,
                             const TempoMap& tempo, double sr, int channel_override = -1)
{
    std::vector<Event> out;
    out.reserve(notes.size() * 2 + controllers.size());
    auto at = [&](Ticks t) { return static_cast<std::int64_t>(std::llround(tempo.seconds_at(t) * sr)); };
    for (const auto& n : notes) {
        const int ch = juce::jlimit(0, 15, channel_override >= 0 ? channel_override : n.channel);
        Event on;
        on.sample = at(n.start);
        on.bytes[0] = static_cast<std::uint8_t>(0x90 | ch);
        on.bytes[1] = static_cast<std::uint8_t>(juce::jlimit(0, 127, n.pitch));
        on.bytes[2] = static_cast<std::uint8_t>(juce::jlimit(1, 127, n.velocity));
        on.order = 1;
        Event off = on;
        off.sample = std::max(on.sample + 1, at(n.end()));
        off.bytes[0] = static_cast<std::uint8_t>(0x80 | ch);
        off.bytes[2] = 0;
        off.order = 0;
        out.push_back(on);
        out.push_back(off);
    }
    for (const auto& c : controllers) {
        Event e;
        e.sample = at(c.tick);
        e.bytes[0] = static_cast<std::uint8_t>(0xB0 | juce::jlimit(0, 15, c.channel));
        e.bytes[1] = static_cast<std::uint8_t>(juce::jlimit(0, 127, c.controller));
        e.bytes[2] = static_cast<std::uint8_t>(juce::jlimit(0, 127, c.value));
        e.order = 0;
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(), [](const Event& a, const Event& b) {
        return a.sample != b.sample ? a.sample < b.sample : a.order < b.order;
    });
    return out;
}

/// Adds the events in [from, from + n) to `midi`, advancing `cursor`.
void gather(const std::vector<Event>& events, std::size_t& cursor, std::int64_t from, int n,
            juce::MidiBuffer& midi) noexcept
{
    const std::int64_t to = from + n;
    while (cursor < events.size() && events[cursor].sample < to) {
        const Event& e = events[cursor++];
        if (e.sample < from) continue;
        midi.addEvent(e.bytes, e.size, static_cast<int>(e.sample - from));
    }
}

// --- Mix plan: routing, processing order, solo --------------------------------------------

struct MixPlan {
    std::vector<const project::Track*> tracks;
    std::vector<const project::Bus*> buses;  // nullptr: a stand-in master when the project has none
    std::vector<std::string> bus_ids;
    std::map<std::string, int> bus_index;
    int master = -1;
    std::vector<int> bus_output;  // -1 for the master
    std::vector<std::vector<int>> bus_sends;
    std::vector<int> track_output;
    std::vector<std::vector<int>> track_sends;
    std::vector<int> order;
    std::vector<bool> track_audible, bus_audible;
    std::vector<bool> track_solo_ok, bus_solo_ok;  // audible but for the strip's own mute
    std::vector<bool> bus_forced_mute;             // a render override mutes it
};

std::vector<const project::Track*> played_tracks(const session::Session& session, bool export_mode)
{
    return export_mode ? session.project().export_tracks() : session.music_tracks();
}

MixPlan make_plan(const project::ProjectData& d, std::vector<const project::Track*> tracks,
                  const PlaybackEngine::RenderOverride* over = nullptr)
{
    MixPlan p;
    p.tracks = std::move(tracks);
    for (const auto& b : d.buses) {
        if (p.bus_index.count(b.id)) continue;
        const int i = static_cast<int>(p.buses.size());
        p.bus_index[b.id] = i;
        p.buses.push_back(&b);
        p.bus_ids.push_back(b.id);
        if (b.kind == "master" && p.master < 0) p.master = i;
    }
    if (p.master < 0) {
        p.master = static_cast<int>(p.buses.size());
        p.bus_index["bus-master"] = p.master;
        p.buses.push_back(nullptr);
        p.bus_ids.emplace_back("bus-master");
    }
    const int n = static_cast<int>(p.buses.size());
    auto find = [&](const std::string& id) {
        auto it = p.bus_index.find(id);
        return it == p.bus_index.end() ? -1 : it->second;
    };
    std::vector<std::vector<int>> adj(static_cast<std::size_t>(n));
    auto reachable = [&](int from, int to) {
        std::vector<char> seen(static_cast<std::size_t>(n), 0);
        std::vector<int> stack{from};
        while (!stack.empty()) {
            const int x = stack.back();
            stack.pop_back();
            if (x == to) return true;
            if (seen[static_cast<std::size_t>(x)]) continue;
            seen[static_cast<std::size_t>(x)] = 1;
            for (int y : adj[static_cast<std::size_t>(x)]) stack.push_back(y);
        }
        return false;
    };
    p.bus_output.assign(static_cast<std::size_t>(n), -1);
    p.bus_sends.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const project::Bus* b = p.buses[static_cast<std::size_t>(i)];
        if (!b) continue;
        auto& sends = p.bus_sends[static_cast<std::size_t>(i)];
        if (i == p.master) {
            sends.assign(b->sends.size(), -1);  // the master feeds the device only
            continue;
        }
        int out = find(b->output);
        if (out < 0 || out == i || reachable(out, i)) out = p.master;  // a loop is ignored
        p.bus_output[static_cast<std::size_t>(i)] = out;
        adj[static_cast<std::size_t>(i)].push_back(out);
        for (const auto& s : b->sends) {
            int dest = find(s.bus_id);
            if (dest >= 0 && (dest == i || reachable(dest, i))) dest = -1;
            sends.push_back(dest);
            if (dest >= 0) adj[static_cast<std::size_t>(i)].push_back(dest);
        }
    }
    // Sources before destinations (Kahn); the master, a sink everything
    // eventually reaches, comes last.
    std::vector<int> indegree(static_cast<std::size_t>(n), 0);
    for (const auto& edges : adj)
        for (int y : edges) ++indegree[static_cast<std::size_t>(y)];
    std::vector<int> ready;
    for (int i = n; --i >= 0;)
        if (indegree[static_cast<std::size_t>(i)] == 0 && i != p.master) ready.push_back(i);
    while (!ready.empty()) {
        const int x = ready.back();
        ready.pop_back();
        p.order.push_back(x);
        for (int y : adj[static_cast<std::size_t>(x)])
            if (--indegree[static_cast<std::size_t>(y)] == 0 && y != p.master) ready.push_back(y);
    }
    p.order.push_back(p.master);

    for (const project::Track* t : p.tracks) {
        const int out = find(t->output_bus_id);
        p.track_output.push_back(out < 0 ? p.master : out);
        std::vector<int> sends;
        for (const auto& s : t->sends) sends.push_back(find(s.bus_id));
        p.track_sends.push_back(std::move(sends));
    }

    // Solo-in-place. A render override replaces the document's solo switches.
    const std::set<std::string> override_solo =
        over ? std::set<std::string>(over->solo.begin(), over->solo.end()) : std::set<std::string>{};
    auto track_solo = [&](const project::Track* t) { return over ? override_solo.count(t->id) > 0 : t->solo; };
    auto bus_solo = [&](const project::Bus* b) { return over ? override_solo.count(b->id) > 0 : b->solo; };
    bool any_solo = over != nullptr;
    for (const auto* t : p.tracks) any_solo = any_solo || track_solo(t);
    for (int i = 0; i < n; ++i)
        if (const auto* b = p.buses[static_cast<std::size_t>(i)]; b && i != p.master) any_solo = any_solo || bus_solo(b);
    std::vector<char> down(static_cast<std::size_t>(n), 0), soloed_bus(static_cast<std::size_t>(n), 0);
    std::vector<int> stack;
    for (std::size_t k = 0; k < p.tracks.size(); ++k)
        if (track_solo(p.tracks[k])) {
            stack.push_back(p.track_output[k]);
            for (int dest : p.track_sends[k])
                if (dest >= 0) stack.push_back(dest);
        }
    for (int i = 0; i < n; ++i)
        if (const auto* b = p.buses[static_cast<std::size_t>(i)]; b && i != p.master && bus_solo(b)) {
            soloed_bus[static_cast<std::size_t>(i)] = 1;
            for (int y : adj[static_cast<std::size_t>(i)]) stack.push_back(y);
        }
    while (!stack.empty()) {
        const int x = stack.back();
        stack.pop_back();
        if (down[static_cast<std::size_t>(x)]) continue;
        down[static_cast<std::size_t>(x)] = 1;
        for (int y : adj[static_cast<std::size_t>(x)]) stack.push_back(y);
    }
    auto feeds_soloed_bus = [&](int out) {
        for (int x = out, guard = 0; x >= 0 && guard <= n; x = p.bus_output[static_cast<std::size_t>(x)], ++guard)
            if (soloed_bus[static_cast<std::size_t>(x)]) return true;
        return false;
    };
    for (std::size_t k = 0; k < p.tracks.size(); ++k) {
        const auto* t = p.tracks[k];
        const bool ok = !any_solo || track_solo(t) || feeds_soloed_bus(p.track_output[k]);
        p.track_solo_ok.push_back(ok);
        p.track_audible.push_back(!t->mute && ok);
    }
    for (int i = 0; i < n; ++i) {
        const auto* b = p.buses[static_cast<std::size_t>(i)];
        if (!b) {
            p.bus_audible.push_back(true);
            p.bus_solo_ok.push_back(true);
            p.bus_forced_mute.push_back(false);
            continue;
        }
        const bool safe = i == p.master || b->kind == "fx";
        const bool ok = !any_solo || safe || bus_solo(b) || down[static_cast<std::size_t>(i)] ||
                        feeds_soloed_bus(p.bus_output[static_cast<std::size_t>(i)]);
        const bool forced = over && over->mute_effects && b->kind == "fx";
        p.bus_solo_ok.push_back(ok);
        p.bus_forced_mute.push_back(forced);
        p.bus_audible.push_back(!b->mute && !forced && ok);
    }
    return p;
}

/// Folds the raw settings into final gains (either thread; no allocation).
void finalize(StripParams& p) noexcept
{
    const bool audible = p.solo_ok && !p.mute;
    const float fader = audible ? db_to_gain(p.gain_db) : 0.0f;
    const auto [pl, pr] = pan_gains(p.pan, p.mono);
    p.out_l = fader * pl;
    p.out_r = fader * pr;
    p.send_l.fill(0.0f);
    p.send_r.fill(0.0f);
    for (std::size_t i = 0; i < static_cast<std::size_t>(p.num_sends); ++i) {
        const auto flags = p.send_flags[i];
        if ((flags & 2) || !audible) continue;
        const float level = db_to_gain(p.send_db[i]) * ((flags & 1) ? 1.0f : fader);
        const auto [sl, sr] = pan_gains((flags & 4) ? p.pan : p.send_pan[i], p.mono);
        p.send_l[i] = level * sl;
        p.send_r[i] = level * sr;
    }
}

StripParams make_params(const project::ChannelStrip& strip, double gain_db, double pan, bool mute, bool solo_ok,
                        bool mono, const std::vector<project::Send>& sends)
{
    StripParams p;
    p.strip = strip;
    p.gain_db = gain_db;
    p.pan = pan;
    p.mute = mute;
    p.solo_ok = solo_ok;
    p.mono = mono;
    p.num_sends = static_cast<int>(std::min(sends.size(), static_cast<std::size_t>(kMaxSends)));
    for (std::size_t i = 0; i < static_cast<std::size_t>(p.num_sends); ++i) {
        const auto& s = sends[i];
        p.send_db[i] = s.level_db;
        p.send_pan[i] = s.pan;
        p.send_flags[i] = static_cast<std::uint8_t>((s.pre_fader ? 1 : 0) | (s.mute ? 2 : 0) | (s.pan_follow ? 4 : 0));
    }
    finalize(p);
    return p;
}

// --- Audio thread helpers ---------------------------------------------------------------

void render_clip(const PlaybackSnapshot::Clip& c, float* l, float* r, std::int64_t pos, int n, bool mono) noexcept
{
    const std::int64_t a = std::max(pos, c.start), b = std::min(pos + n, c.start + c.length);
    if (b <= a) return;
    constexpr float half_pi = juce::MathConstants<float>::halfPi;
    constexpr int kChunk = 256;
    std::array<float, kChunk> gains{};
    for (std::int64_t from = a; from < b; from += kChunk) {
        const int count = static_cast<int>(std::min<std::int64_t>(kChunk, b - from));
        for (int i = 0; i < count; ++i) {
            const std::int64_t k = from + i - c.start;
            const std::int64_t remaining = c.length - 1 - k;
            float g = c.gain;
            if (k < c.fade_in) g *= std::sin(half_pi * (static_cast<float>(k) + 0.5f) / static_cast<float>(c.fade_in));
            if (remaining < c.fade_out)
                g *= std::sin(half_pi * (static_cast<float>(remaining) + 0.5f) / static_cast<float>(c.fade_out));
            if (k < c.declick) g *= (static_cast<float>(k) + 0.5f) / static_cast<float>(c.declick);
            if (remaining < c.declick) g *= (static_cast<float>(remaining) + 0.5f) / static_cast<float>(c.declick);
            gains[static_cast<std::size_t>(i)] = g;
        }
        const auto at = static_cast<std::size_t>(from - pos);
        c.stream->read(from - c.start, count, gains.data(), l + at, r + at, mono && c.stream->channels() > 1);
    }
}

/// Audio thread, while stopped: positions a clip's stream for a play from
/// `pos`, so starting playback finds its audio already read.
void cue_clip(const PlaybackSnapshot::Clip& c, std::int64_t pos) noexcept
{
    if (pos < c.start + c.length) c.stream->cue(std::max<std::int64_t>(0, pos - c.start));
}

/// Audio thread, before a strip's instrument and processing: evaluates its
/// automation for the block [pos, pos + n). Gains take the lane's value at the
/// end of the block (process_strip ramps to it); strip, insert and plug-in
/// parameters take the value at the start.
void automate_strip(PlaybackSnapshot::Strip& st, std::int64_t pos, int n, bool playing) noexcept
{
    StripDsp& d = *st.dsp;
    d.use_eff = false;
    d.mute_mode = 0;
    if (st.autos.empty()) return;
    d.eff = d.params.latest();
    bool changed = false;
    for (auto& ap : st.autos) {
        const int mode = ap.slot ? ap.slot->mode.load(std::memory_order_relaxed) : 0;
        if (mode == 2) continue;  // write mode, untouched: the static value
        const bool gain = ap.kind == AutoKind::Volume || ap.kind == AutoKind::Pan || ap.kind == AutoKind::Send;
        const std::int64_t t = playing ? pos + (gain ? n : 0) - ap.offset : pos;
        if (ap.kind == AutoKind::Mute) {
            changed = true;
            d.eff.mute = false;
            if (mode == 1) {
                d.mute_mode = 2;
                d.mute_target = ap.slot->value.load(std::memory_order_relaxed) >= 0.5 ? 0.0f : 1.0f;
            } else if (!ap.curve.empty()) {
                d.mute_mode = 1;
                d.mute_curve = &ap.curve;
                d.mute_from = playing ? pos - ap.offset : pos;
            } else {
                d.eff.mute = d.params.latest().mute;
            }
            continue;
        }
        double v = 0.0;
        if (mode == 1) v = ap.slot->value.load(std::memory_order_relaxed);
        else if (!ap.curve.empty()) v = curve_value(ap.curve, t);
        else continue;
        switch (ap.kind) {
            case AutoKind::Volume: d.eff.gain_db = v; changed = true; break;
            case AutoKind::Pan: d.eff.pan = v; changed = true; break;
            case AutoKind::Send:
                if (ap.a >= 0 && ap.a < kMaxSends) d.eff.send_db[static_cast<std::size_t>(ap.a)] = v;
                changed = true;
                break;
            case AutoKind::Strip: set_strip_field(d.eff.strip, ap.a, v); changed = true; break;
            case AutoKind::Insert:
                if (ap.a >= 0 && ap.a < static_cast<int>(st.inserts.size()))
                    st.inserts[static_cast<std::size_t>(ap.a)]->automate(ap.b, static_cast<float>(v));
                break;
            case AutoKind::Plugin: {
                const float f = juce::jlimit(0.0f, 1.0f, static_cast<float>(v));
                if (ap.param && std::abs(f - ap.last_sent) > 1.0e-6f) {
                    ap.param->setValue(f);
                    ap.last_sent = f;
                }
                break;
            }
            case AutoKind::Mute: break;
        }
    }
    if (changed) {
        finalize(d.eff);
        d.use_eff = true;
    }
}

/// Channel strip, inserts, sends, fader/pan and meters; adds the result to
/// the strip's output (a bus, or the device for the master), through the
/// edge's compensation delay unless `low_latency`.
void process_strip(PlaybackSnapshot& s, PlaybackSnapshot::Strip& strip, juce::AudioBuffer<float>& buf, int n,
                   bool playing, bool low_latency) noexcept
{
    StripDsp& d = *strip.dsp;
    const StripParams& p = d.use_eff ? d.eff : d.params.latest();
    float* l = buf.getWritePointer(0);
    float* r = buf.getWritePointer(1);
    d.channel.process(l, r, n, p.strip);
    for (const auto& ins : strip.inserts) {
        if (!ins->bypass.load(std::memory_order_relaxed)) ins->process(l, r, n);
        else ins->process_bypassed(l, r, n);
    }
    // Automated mute: before the sends and the fader, like the static mute.
    if (d.mute_mode == 1 && d.mute_curve) {
        if (playing) {
            apply_curve_gain(*d.mute_curve, d.mute_from, l, r, n);
            d.mute_cur = static_cast<float>(curve_value(*d.mute_curve, d.mute_from + n - 1));
        } else {
            d.mute_cur = static_cast<float>(curve_value(*d.mute_curve, d.mute_from));
            buf.applyGain(d.mute_cur);
        }
    } else if (d.mute_mode == 2) {
        buf.applyGainRamp(0, n, d.mute_cur, d.mute_target);
        d.mute_cur = d.mute_target;
    } else {
        d.mute_cur = 1.0f;
    }
    if (!d.primed) {
        d.primed = true;
        d.cur_l = p.out_l;
        d.cur_r = p.out_r;
        d.cur_send_l = p.send_l;
        d.cur_send_r = p.send_r;
    }
    const std::size_t sends = std::min(strip.sends.size(), static_cast<std::size_t>(kMaxSends));
    for (std::size_t i = 0; i < sends; ++i) {
        const int dest = strip.sends[i];
        if (dest < 0 || dest >= static_cast<int>(s.buses.size())) continue;
        auto& target = s.buses[static_cast<std::size_t>(dest)].buffer;
        DelayLine* line = strip.send_delay[i] > 0 ? strip.send_lines[i].get() : nullptr;
        if (line && !low_latency) {
            line->send(l, r, n, d.cur_send_l[i], p.send_l[i], d.cur_send_r[i], p.send_r[i], target.getWritePointer(0),
                       target.getWritePointer(1), strip.send_delay[i]);
        } else {
            if (line) line->silence(n);
            if (d.cur_send_l[i] > 0.0f || p.send_l[i] > 0.0f) target.addFromWithRamp(0, 0, l, n, d.cur_send_l[i], p.send_l[i]);
            if (d.cur_send_r[i] > 0.0f || p.send_r[i] > 0.0f) target.addFromWithRamp(1, 0, r, n, d.cur_send_r[i], p.send_r[i]);
        }
        d.cur_send_l[i] = p.send_l[i];
        d.cur_send_r[i] = p.send_r[i];
    }
    buf.applyGainRamp(0, 0, n, d.cur_l, p.out_l);
    buf.applyGainRamp(1, 0, n, d.cur_r, p.out_r);
    d.cur_l = p.out_l;
    d.cur_r = p.out_r;
    // The output edge's compensation; metered after it, so meters follow the sound.
    if (strip.out_delay > 0 && strip.out_line) {
        if (low_latency) strip.out_line->silence(n);
        else strip.out_line->process(l, r, n, strip.out_delay);
    }

    const float pl = buf.getMagnitude(0, 0, n), pr = buf.getMagnitude(1, 0, n);
    store_max(d.peak_l, pl);
    store_max(d.peak_r, pr);
    if (pl > 1.0f || pr > 1.0f) d.clipped.store(true, std::memory_order_relaxed);
    if (p.strip.comp_on) store_min(d.gr_db, d.channel.gain_reduction_db());
    if (p.strip.gate_on && d.channel.gate_closed()) d.gate_closed.store(true, std::memory_order_relaxed);

    juce::AudioBuffer<float>& dest = strip.output >= 0 && strip.output < static_cast<int>(s.buses.size())
                                         ? s.buses[static_cast<std::size_t>(strip.output)].buffer
                                         : s.out;
    dest.addFrom(0, 0, buf, 0, 0, n);
    dest.addFrom(1, 0, buf, 1, 0, n);
}

}  // namespace

// --- Engine ------------------------------------------------------------------------------

class LiveMidiCallback final : public juce::MidiInputCallback {
public:
    explicit LiveMidiCallback(juce::MidiMessageCollector& c) : collector_(c) {}
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m) override { collector_.addMessageToQueue(m); }

private:
    juce::MidiMessageCollector& collector_;
};

PlaybackEngine::PlaybackEngine(session::Session& session, PluginHost& plugins)
    : session_(session), plugins_(plugins), files_(std::make_unique<DiskStreamer>())
{
    for (auto& m : input_meters_) m.store(0.0f);
    listener_ = session_.add_listener([this](session::Change c) {
        if (c != session::Change::Document) return;
        sync_processors();  // mix changes are heard at once; structure follows with the rebuild
        request_rebuild();
        if (writing_pass_ && !committing_) refresh_writes();
    });
    midi_callback_ = std::make_unique<LiveMidiCallback>(live_input_);
    live_input_.reset(sample_rate_);
    rebuild_now();
    startTimerHz(30);
}

PlaybackEngine::~PlaybackEngine()
{
    stopTimer();
    session_.remove_listener(listener_);
    devices_.removeAudioCallback(this);
    devices_.removeMidiInputDeviceCallback({}, midi_callback_.get());
    devices_.closeAudioDevice();
    // Unfinished recordings: close the files (they stay on disk), place nothing.
    delete pending_pass_.exchange(nullptr);
    delete finished_pass_.exchange(nullptr);
    delete active_pass_;
    active_pass_ = nullptr;
    delete pending_.exchange(nullptr);
    delete current_;
    current_ = nullptr;
    delete spill_;
    spill_ = nullptr;
    delete pending_preview_.exchange(nullptr);
    delete preview_;
    preview_ = nullptr;
    retire_garbage();
}

juce::String PlaybackEngine::open_default_device()
{
    // Inputs for recording and monitoring; without any, play only.
    juce::String error = devices_.initialiseWithDefaultDevices(2, 2);
    if (error.isNotEmpty() || devices_.getCurrentAudioDevice() == nullptr) error = devices_.initialiseWithDefaultDevices(0, 2);
    if (error.isEmpty() && devices_.getCurrentAudioDevice() == nullptr) error = "no audio output device was found";
    if (error.isEmpty()) {
        auto* device = devices_.getCurrentAudioDevice();
        const int available = device->getInputChannelNames().size();
        if (available > device->getActiveInputChannels().countNumberOfSetBits()) {
            auto setup = devices_.getAudioDeviceSetup();
            setup.useDefaultInputChannels = false;
            setup.inputChannels.clear();
            setup.inputChannels.setRange(0, std::min(available, static_cast<int>(input_meters_.size())), true);
            devices_.setAudioDeviceSetup(setup, true);  // keeps the default inputs if this fails
        }
        devices_.addAudioCallback(this);
    }
    for (const auto& input : juce::MidiInput::getAvailableDevices()) {
        devices_.setMidiInputDeviceEnabled(input.identifier, true);
    }
    devices_.addMidiInputDeviceCallback({}, midi_callback_.get());
    return error;
}

void PlaybackEngine::prepare_offline(double sample_rate, int block_size)
{
    files_->set_synchronous(true);  // offline: reads fill their own buffers
    prepare_all(sample_rate, block_size);
    latency_samples_ = 0;
    rebuild_now();
}

void PlaybackEngine::render_offline(juce::AudioBuffer<float>& out, int num_samples)
{
    out.clear();
    process(nullptr, 0, out.getArrayOfWritePointers(), out.getNumChannels(), num_samples);
    retire_garbage();
}

void PlaybackEngine::render_offline_with_input(const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& out,
                                               int num_samples)
{
    out.clear();
    num_inputs_.store(input.getNumChannels());
    const int n = std::min(num_samples, input.getNumSamples());
    process(input.getArrayOfReadPointers(), input.getNumChannels(), out.getArrayOfWritePointers(), out.getNumChannels(), n);
    retire_garbage();
}

void PlaybackEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    files_->set_synchronous(false);
    // Called before callbacks (re)start, on the thread that opened the
    // device, so instruments can be re-prepared without racing the audio thread.
    prepare_all(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
    latency_samples_ = device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();
    num_inputs_ = device->getActiveInputChannels().countNumberOfSetBits();
    device_running_ = true;
    rebuild_now();
}

void PlaybackEngine::audioDeviceStopped() { device_running_ = false; }

void PlaybackEngine::prepare_all(double sr, int bs)
{
    sample_rate_ = sr;
    block_size_ = std::max(bs, 64);
    live_input_.reset(sr);
    // Processors are re-prepared here (state is kept when the rate is
    // unchanged); the snapshot that refers to them is replaced immediately
    // after, and the audio thread ignores a snapshot whose block size it
    // cannot honour.
    for (auto& [id, slot] : instruments_)
        if (slot.instrument) slot.instrument->prepare(sample_rate_, block_size_);
    for (auto& [id, strip] : strips_) strip->channel.prepare(sample_rate_);
    for (auto& [key, insert] : inserts_) insert->prepare(sample_rate_, block_size_);
    // Streams are keyed by rate; ones at the old rate go when unused.
}

void PlaybackEngine::request_rebuild() { rebuild_pending_ = true; }

void PlaybackEngine::timerCallback()
{
    drain_gestures();
    // A plug-in that changed its latency (setLatencySamples) re-aligns the mix.
    if (rebuild_pending_ || current_latencies() != latencies_seen_) rebuild_now();
    retire_garbage();
    collect_finished_passes();
}

void PlaybackEngine::retire_garbage()
{
    PlaybackSnapshot* s = nullptr;
    while (retired_.pop(s)) delete s;
    PreviewClip* c = nullptr;
    while (retired_previews_.pop(c)) delete c;
}

std::shared_ptr<Instrument> PlaybackEngine::instrument_for(const project::Track& t, InstrumentSlot& slot)
{
    const auto* def = session_.instrument_for(t);
    const PlaceholderVoice voice = placeholder_voice_for(def ? def->family : t.family,
                                                         def ? def->gm_program : t.assignment.placeholder_program,
                                                         (def && def->unpitched) || t.midi_channel == 9);
    std::string key = t.instrument_plugin.empty() ? std::string("placeholder:") + to_string(voice)
                                                  : "plugin:" + t.instrument_plugin.identifier;
    if (slot.instrument && slot.key == key) return slot.instrument;

    slot.key = key;
    if (!t.instrument_plugin.empty()) {
        juce::String error;
        auto instance = plugins_.create(t.instrument_plugin, sample_rate_, block_size_, error);
        if (instance) {
            auto inst = std::make_shared<PluginInstrument>(std::move(instance));
            inst->prepare(sample_rate_, block_size_);
            slot.instrument = inst;
            slot.status = inst->display_name();
            return slot.instrument;
        }
        // Keep the track playing on the placeholder and say why (Main Spec §15).
        slot.status = "Missing: " + juce::String(t.instrument_plugin.name) + " (" + error + ") - placeholder playing";
    }
    auto synth = std::make_shared<PlaceholderSynth>(voice);
    synth->prepare(sample_rate_, block_size_);
    if (t.instrument_plugin.empty()) slot.status = synth->display_name();
    slot.instrument = synth;
    return slot.instrument;
}

void PlaybackEngine::sync_processors()
{
    const auto& d = session_.data();
    const MixPlan plan = make_plan(d, played_tracks(session_, export_mode_), render_override_ ? &*render_override_ : nullptr);
    std::set<std::string> live_strips, live_inserts;

    auto strip_for = [&](const std::string& id) -> StripDsp& {
        auto& slot = strips_[id];
        if (!slot) {
            slot = std::make_shared<StripDsp>();
            slot->channel.prepare(sample_rate_);
        }
        live_strips.insert(id);
        return *slot;
    };
    insert_watch_targets_.clear();
    auto sync_inserts = [&](const std::string& id, const std::vector<project::InsertSlot>& slots) {
        auto& keys = insert_keys_[id];
        keys.assign(slots.size(), {});
        std::map<std::string, int> occurrences;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const auto& slot = slots[i];
            if (!insert_active(slot)) continue;
            // Keyed by kind (and plug-in) and occurrence, not position, so
            // removing an earlier insert does not reset a later one.
            const std::string base = id + "/" + slot.kind + "/" + slot.plugin.identifier;
            const std::string key = base + "#" + std::to_string(occurrences[base]++);
            auto& proc = inserts_[key];
            if (!proc) proc = create_insert(slot, plugins_, sample_rate_, block_size_);
            if (!proc) continue;
            proc->set_params(slot);
            proc->bypass.store(slot.bypass);
            if (auto* instance = proc->plugin()) {
                InsertProcessor* raw = proc.get();
                watch_plugin(*instance, raw, raw->has_watch(),
                             [raw](std::unique_ptr<ParameterWatch> w) { raw->set_watch(std::move(w)); },
                             insert_watch_targets_, id, "insert_plugin:" + std::to_string(i) + ":");
            }
            keys[i] = key;
            live_inserts.insert(key);
        }
    };

    for (std::size_t k = 0; k < plan.tracks.size(); ++k) {
        const project::Track& t = *plan.tracks[k];
        strip_for(t.id).params.write(
            make_params(t.strip, t.gain_db, t.pan, t.mute, plan.track_solo_ok[k], is_mono_track(t), t.sends));
        sync_inserts(t.id, t.inserts);
    }
    for (std::size_t i = 0; i < plan.buses.size(); ++i) {
        const project::Bus* b = plan.buses[i];
        StripDsp& strip = strip_for(plan.bus_ids[i]);
        if (!b) {
            strip.params.write(make_params({}, 0.0, 0.0, false, true, false, {}));
            continue;
        }
        strip.params.write(make_params(b->strip, b->gain_db, b->pan, b->mute || plan.bus_forced_mute[i],
                                       plan.bus_solo_ok[i], false, b->sends));
        sync_inserts(b->id, b->inserts);
    }
    // Strips and inserts that are gone: snapshots still holding them keep
    // them alive until they are retired on this thread.
    for (auto it = strips_.begin(); it != strips_.end();) {
        if (!live_strips.count(it->first)) it = strips_.erase(it);
        else ++it;
    }
    for (auto it = insert_keys_.begin(); it != insert_keys_.end();) {
        if (!live_strips.count(it->first)) it = insert_keys_.erase(it);
        else ++it;
    }
    for (auto it = inserts_.begin(); it != inserts_.end();) {
        if (!live_inserts.count(it->first)) it = inserts_.erase(it);
        else ++it;
    }
}

std::unique_ptr<PlaybackSnapshot> PlaybackEngine::build_snapshot()
{
    sync_processors();
    const auto& d = session_.data();
    const MixPlan plan = make_plan(d, played_tracks(session_, export_mode_), render_override_ ? &*render_override_ : nullptr);
    auto snap = std::make_unique<PlaybackSnapshot>();
    std::map<std::string, std::shared_ptr<ClipStream>> live_streams;
    snap->sample_rate = sample_rate_;
    snap->max_block = block_size_;
    snap->out.setSize(2, block_size_);

    auto make_strip = [&](const std::string& id, std::vector<int> sends, int output) {
        PlaybackSnapshot::Strip st;
        st.dsp = strips_[id];
        for (const auto& key : insert_keys_[id])
            if (!key.empty())
                if (auto it = inserts_.find(key); it != inserts_.end() && it->second) st.inserts.push_back(it->second);
        st.sends = std::move(sends);
        st.output = output;
        return st;
    };

    snap->buses.resize(plan.buses.size());
    for (std::size_t i = 0; i < plan.buses.size(); ++i) {
        auto& sb = snap->buses[i];
        sb.strip = make_strip(plan.bus_ids[i], plan.bus_sends[i], plan.bus_output[i]);
        sb.buffer.setSize(2, block_size_);
    }
    snap->bus_order = plan.order;

    std::set<std::string> instrument_ids;
    audio_status_.clear();
    for (std::size_t k = 0; k < plan.tracks.size(); ++k) {
        const project::Track* t = plan.tracks[k];
        PlaybackSnapshot::Track st;
        st.strip = make_strip(t->id, plan.track_sends[k], plan.track_output[k]);
        if (has_instrument(*t)) {
            instrument_ids.insert(t->id);
            auto& slot = instruments_[t->id];
            st.instrument = instrument_for(*t, slot);
            if (auto* pi = dynamic_cast<PluginInstrument*>(st.instrument.get()))
                watch_plugin(pi->instance(), pi, pi->has_watch(),
                             [pi](std::unique_ptr<ParameterWatch> w) { pi->set_watch(std::move(w)); },
                             instrument_watch_targets_, t->id, "plugin:");
            project::RenderedTrack r = project::render_track(*t);
            if (st.instrument->wants_keyswitches()) r.notes.insert(r.notes.end(), r.keyswitches.begin(), r.keyswitches.end());
            st.events = to_events(r.notes, r.controllers, d.tempo, sample_rate_);
            st.midi.ensureSize(4096);
        }
        if (const project::TrackVersion* v = t->active_version()) {
            juce::StringArray problems;
            for (const auto& c : v->audio_clips) {
                if (c.lane != 0 || c.muted || c.length <= 0.0) continue;
                const std::int64_t length = std::llround(c.length * sample_rate_);
                const std::int64_t offset = std::llround(std::max(0.0, c.source_offset) * sample_rate_);
                const std::string path = session_.resolve_audio_path(c.file);
                // The same clip keeps its stream (and its buffered audio) across rebuilds.
                const std::string key = t->id + "|" + c.id + "|" + path + "|" + std::to_string(offset) + "|" +
                                        std::to_string(length) + "|" + std::to_string(static_cast<long long>(sample_rate_));
                std::shared_ptr<ClipStream> stream;
                if (auto it = streams_.find(key); it != streams_.end()) {
                    stream = it->second;
                } else {
                    juce::String error;
                    stream = files_->open(path, sample_rate_, offset, length, error);
                    if (!stream) {
                        problems.addIfNotAlreadyThere(error);
                        continue;
                    }
                }
                live_streams[key] = stream;
                PlaybackSnapshot::Clip clip;
                clip.stream = std::move(stream);
                clip.start = std::llround(c.start_seconds * sample_rate_);
                clip.length = length;
                clip.offset = offset;
                clip.fade_in = std::llround(std::max(0.0, c.fade_in) * sample_rate_);
                clip.fade_out = std::llround(std::max(0.0, c.fade_out) * sample_rate_);
                clip.declick = std::max<std::int64_t>(1, std::llround(kDeclickSeconds * sample_rate_));
                clip.gain = db_to_gain(c.gain_db);
                st.clips.push_back(std::move(clip));
            }
            if (!problems.isEmpty()) audio_status_[t->id] = problems.joinIntoString("; ");
        }
        st.mono = is_mono_track(*t);
        st.monitor = t->monitor == "off" ? 0 : t->monitor == "input" ? 2 : 1;
        st.armed = t->record_armed;
        st.input = t->input;
        st.input_stereo = t->input_stereo;
        st.buffer.setSize(2, block_size_);
        if (t->id == live_track_) snap->live_track = static_cast<int>(snap->tracks.size());
        snap->tracks.push_back(std::move(st));
    }
    // Instruments of deleted tracks: dropped here; snapshots still holding
    // them keep them alive until they are retired on this thread.
    for (auto it = instruments_.begin(); it != instruments_.end();) {
        if (!instrument_ids.count(it->first)) it = instruments_.erase(it);
        else ++it;
    }

    // --- Plug-in delay compensation ------------------------------------------------
    // Latency of each strip (instrument + inserts; a bypassed plug-in keeps
    // its latency), then, sources before destinations, when each bus's
    // inputs arrive and how long each routing edge must wait to match.
    auto inserts_latency = [](const PlaybackSnapshot::Strip& st, std::size_t upto) {
        int sum = 0;
        for (std::size_t i = 0; i < upto && i < st.inserts.size(); ++i) sum += std::max(0, st.inserts[i]->latency_samples());
        return sum;
    };
    const std::size_t nb = snap->buses.size();
    std::vector<int> instrument_latency(snap->tracks.size(), 0), track_out(snap->tracks.size(), 0);
    std::vector<int> bus_own(nb, 0), bus_in(nb, 0), bus_out(nb, 0);
    strip_latency_.clear();
    for (std::size_t k = 0; k < snap->tracks.size(); ++k) {
        const auto& st = snap->tracks[k];
        instrument_latency[k] = st.instrument ? std::max(0, st.instrument->latency_samples()) : 0;
        track_out[k] = instrument_latency[k] + inserts_latency(st.strip, st.strip.inserts.size());
        strip_latency_[plan.tracks[k]->id] = track_out[k];
        auto arrive = [&](int dest) {
            if (dest >= 0 && dest < static_cast<int>(nb)) bus_in[static_cast<std::size_t>(dest)] = std::max(bus_in[static_cast<std::size_t>(dest)], track_out[k]);
        };
        arrive(st.strip.output);
        for (std::size_t i = 0; i < st.strip.sends.size() && i < static_cast<std::size_t>(kMaxSends); ++i) arrive(st.strip.sends[i]);
    }
    for (int bi : snap->bus_order) {
        const auto b = static_cast<std::size_t>(bi);
        const auto& st = snap->buses[b].strip;
        bus_own[b] = inserts_latency(st, st.inserts.size());
        bus_out[b] = bus_in[b] + bus_own[b];
        strip_latency_[plan.bus_ids[b]] = bus_own[b];
        auto arrive = [&](int dest) {
            if (dest >= 0 && dest < static_cast<int>(nb)) bus_in[static_cast<std::size_t>(dest)] = std::max(bus_in[static_cast<std::size_t>(dest)], bus_out[b]);
        };
        arrive(st.output);
        for (std::size_t i = 0; i < st.sends.size() && i < static_cast<std::size_t>(kMaxSends); ++i) arrive(st.sends[i]);
    }
    const int master = plan.master;
    const int total = master >= 0 && master < static_cast<int>(nb) ? bus_out[static_cast<std::size_t>(master)] : 0;
    compensation_ = pdc_ ? total : 0;
    snap->comp = compensation_;
    std::set<std::string> live_lines;
    auto line_for = [&](const std::string& key, int delay) -> std::shared_ptr<DelayLine> {
        if (delay <= 0) return nullptr;
        live_lines.insert(key);
        auto& line = delay_lines_[key];
        if (!line || line->capacity() < delay + block_size_ + 1) line = std::make_shared<DelayLine>(delay + block_size_ + 1);
        return line;
    };
    auto set_edges = [&](PlaybackSnapshot::Strip& st, const std::string& id, int out_latency) {
        auto edge = [&](int dest) {
            if (!pdc_ || dest < 0 || dest >= static_cast<int>(nb)) return 0;
            return bus_in[static_cast<std::size_t>(dest)] - out_latency;
        };
        st.out_delay = edge(st.output);
        st.out_line = line_for(id + ">out", st.out_delay);
        for (std::size_t i = 0; i < st.sends.size() && i < static_cast<std::size_t>(kMaxSends); ++i) {
            st.send_delay[i] = edge(st.sends[i]);
            st.send_lines[i] = line_for(id + ">send" + std::to_string(i), st.send_delay[i]);
        }
    };
    for (std::size_t k = 0; k < snap->tracks.size(); ++k) set_edges(snap->tracks[k].strip, plan.tracks[k]->id, track_out[k]);
    for (std::size_t b = 0; b < nb; ++b) set_edges(snap->buses[b].strip, plan.bus_ids[b], bus_out[b]);
    for (auto it = delay_lines_.begin(); it != delay_lines_.end();) {
        if (!live_lines.count(it->first)) it = delay_lines_.erase(it);
        else ++it;
    }
    latencies_seen_ = current_latencies();

    // --- Automation -------------------------------------------------------------------
    const std::int64_t mute_fade = std::max<std::int64_t>(1, std::llround(kMuteFadeSeconds * sample_rate_));
    auto automate = [&](PlaybackSnapshot::Strip& st, const std::string& owner, const std::string& mode,
                        const std::vector<project::AutomationLane>& lanes, const std::vector<project::Send>& sends,
                        const std::vector<project::InsertSlot>& slots, Instrument* instrument, std::int64_t pre,
                        std::int64_t post) {
        // Slot index -> index into st.inserts (inactive slots have none).
        std::vector<int> slot_insert(slots.size(), -1);
        {
            const auto& keys = insert_keys_[owner];
            int next = 0;
            for (std::size_t i = 0; i < keys.size() && i < slots.size(); ++i)
                if (!keys[i].empty() && inserts_.count(keys[i]) && inserts_[keys[i]]) slot_insert[i] = next++;
        }
        std::vector<std::string> params;
        if (mode != "off")
            for (const auto& lane : lanes)
                if (lane.enabled && !lane.points.empty()) params.push_back(lane.param);
        // Parameters written in this pass without a lane yet.
        const std::string prefix = owner + '\x1f';
        for (auto it = auto_slots_.lower_bound(prefix); it != auto_slots_.end() && it->first.rfind(prefix, 0) == 0; ++it) {
            const std::string param = it->first.substr(prefix.size());
            if (std::find(params.begin(), params.end(), param) == params.end()) params.push_back(param);
        }
        for (const auto& param : params) {
            AutoParam ap;
            const project::AutomationLane* lane = mode == "off" ? nullptr : project::find_lane(lanes, param);
            if (lane && (!lane->enabled || lane->points.empty())) lane = nullptr;
            const auto info = project::param_info(param);
            if (!info) continue;
            ap.offset = post;
            if (param == "volume") ap.kind = AutoKind::Volume;
            else if (param == "pan") ap.kind = AutoKind::Pan;
            else if (param == "mute") ap.kind = AutoKind::Mute;
            else if (param.rfind("send:", 0) == 0) {
                ap.kind = AutoKind::Send;
                for (std::size_t i = 0; i < sends.size(); ++i)
                    if (sends[i].bus_id == param.substr(5)) ap.a = static_cast<int>(i);
                if (ap.a < 0 || ap.a >= kMaxSends) continue;
            } else if (param.rfind("strip.", 0) == 0) {
                ap.kind = AutoKind::Strip;
                ap.a = strip_field_index(param);
                ap.offset = pre;
                if (ap.a < 0) continue;
            } else {
                // insert:<slot>:<name>, plugin:<index>, insert_plugin:<slot>:<index>
                std::vector<std::string> parts;
                for (std::size_t from = 0;;) {
                    const auto at = param.find(':', from);
                    parts.push_back(param.substr(from, at == std::string::npos ? std::string::npos : at - from));
                    if (at == std::string::npos) break;
                    from = at + 1;
                }
                auto number = [](const std::string& x) { return x.empty() ? -1 : std::atoi(x.c_str()); };
                if (parts[0] == "plugin" && parts.size() == 2) {
                    auto* pi = dynamic_cast<PluginInstrument*>(instrument);
                    const int index = number(parts[1]);
                    if (!pi || index < 0 || index >= pi->instance().getParameters().size()) continue;
                    ap.kind = AutoKind::Plugin;
                    ap.param = pi->instance().getParameters()[index];
                    ap.offset = 0;
                } else if ((parts[0] == "insert" || parts[0] == "insert_plugin") && parts.size() == 3) {
                    const int slot = number(parts[1]);
                    if (slot < 0 || slot >= static_cast<int>(slots.size())) continue;
                    const int ins = slot_insert[static_cast<std::size_t>(slot)];
                    if (ins < 0) continue;
                    ap.a = ins;
                    ap.offset = pre + inserts_latency(st, static_cast<std::size_t>(ins));
                    if (parts[0] == "insert") {
                        ap.kind = AutoKind::Insert;
                        ap.b = insert_automation_index(slots[static_cast<std::size_t>(slot)].kind, parts[2]);
                        if (ap.b < 0) continue;
                    } else {
                        auto* instance = st.inserts[static_cast<std::size_t>(ins)]->plugin();
                        const int index = number(parts[2]);
                        if (!instance || index < 0 || index >= instance->getParameters().size()) continue;
                        ap.kind = AutoKind::Plugin;
                        ap.param = instance->getParameters()[index];
                    }
                } else {
                    continue;
                }
            }
            if (lane)
                ap.curve = ap.kind == AutoKind::Mute ? make_mute_gain_curve(*lane, d.tempo, sample_rate_, mute_fade)
                                                     : make_curve(*lane, d.tempo, sample_rate_, info->toggle);
            ap.slot = slot_for(owner, param);
            st.autos.push_back(std::move(ap));
        }
    };
    for (std::size_t k = 0; k < snap->tracks.size(); ++k) {
        const project::Track& t = *plan.tracks[k];
        auto& st = snap->tracks[k];
        automate(st.strip, t.id, t.automation_mode, t.automation, t.sends, t.inserts, st.instrument.get(),
                 instrument_latency[k], track_out[k]);
    }
    for (std::size_t b = 0; b < nb; ++b) {
        const project::Bus* bus = plan.buses[b];
        if (!bus) continue;
        const int pre = pdc_ ? bus_in[b] : 0;
        automate(snap->buses[b].strip, plan.bus_ids[b], bus->automation_mode, bus->automation, bus->sends, bus->inserts,
                 nullptr, pre, pre + bus_own[b]);
    }

    // Click: every felt beat to the end of the content.
    const Ticks end = session_.content_end();
    for (int bar = 1;; ++bar) {
        const Ticks start = d.meter.bar_start(bar);
        if (start >= end) break;
        const TimeSignature sig = d.meter.at_bar(bar);
        const Ticks beat = sig.is_compound() ? sig.beat_ticks() * 3 : sig.beat_ticks();
        for (Ticks t = start, i = 0; t < start + sig.ticks_per_bar(); t += beat, ++i)
            snap->clicks.emplace_back(static_cast<std::int64_t>(std::llround(d.tempo.seconds_at(t) * sample_rate_)), i == 0);
    }
    snap->seek(position_samples_.load());
    // Streams no clip uses any more go when the snapshots holding them retire.
    streams_ = std::move(live_streams);
    return snap;
}

void PlaybackEngine::rebuild_now()
{
    rebuild_pending_ = false;
    auto snap = build_snapshot();
    delete pending_.exchange(snap.release());  // an unadopted older one is simply replaced
}

void PlaybackEngine::play()
{
    const bool was = playing_.load();
    play_request_ = true;
    playing_ = true;
    begin_pass();
    if (!was) start_automation_pass();
    if (on_transport_changed) on_transport_changed();
}

void PlaybackEngine::stop()
{
    if (playing_.load()) finish_automation_pass();  // at the heard position, before it rewinds
    stop_request_ = true;
    playing_ = false;
    end_passes();
    if (on_transport_changed) on_transport_changed();
}

void PlaybackEngine::toggle_play() { playing() ? stop() : play(); }

void PlaybackEngine::locate(Ticks tick)
{
    const auto sample = static_cast<std::int64_t>(std::llround(session_.data().tempo.seconds_at(std::max<Ticks>(0, tick)) * sample_rate_));
    const bool restart = playing_.load() && writing_pass_;
    if (restart) finish_automation_pass();  // a jump ends the written pass
    locate_request_ = sample;
    position_samples_ = sample;  // the UI sees the new position immediately
    heard_samples_ = sample;
    gestures_->heard.store(sample);
    if (restart) start_automation_pass();
    if (on_transport_changed) on_transport_changed();
}

double PlaybackEngine::position_seconds() const { return static_cast<double>(heard_samples_.load()) / sample_rate_; }

Ticks PlaybackEngine::position() const { return session_.data().tempo.tick_at_seconds(position_seconds()); }

void PlaybackEngine::preview(const std::vector<NoteEvent>& notes, const std::string& family, int gm_program, double bpm,
                             bool loop, Ticks loop_length)
{
    auto clip = std::make_unique<PreviewClip>();
    clip->synth = std::make_unique<PlaceholderSynth>(placeholder_voice_for(family, gm_program, false));
    clip->synth->prepare(sample_rate_, block_size_);
    TempoMap tempo(bpm > 0 ? bpm : 100.0);
    clip->events = to_events(notes, {}, tempo, sample_rate_, 0);
    Ticks end = loop_length;
    for (const auto& n : notes) end = std::max(end, n.end());
    clip->length = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(tempo.seconds_at(end) * sample_rate_)));
    clip->loop = loop;
    clip->midi.ensureSize(4096);
    clip->buffer.setSize(2, block_size_);
    stop_preview_request_ = false;
    previewing_ = true;
    delete pending_preview_.exchange(clip.release());
}

void PlaybackEngine::stop_preview()
{
    stop_preview_request_ = true;
    delete pending_preview_.exchange(nullptr);
}

juce::String PlaybackEngine::instrument_status(const std::string& id) const
{
    auto it = instruments_.find(id);
    if (it == instruments_.end()) return "No instrument";
    if (auto* pi = dynamic_cast<PluginInstrument*>(it->second.instrument.get()))
        if (auto* remote = dynamic_cast<RemotePluginInstance*>(&pi->instance()); remote && remote->crashed())
            return "Crashed: " + remote->getName().upToLastOccurrenceOf(" (crashed)", false, false) +
                   " - Tools > Reload Crashed Plug-ins";
    return it->second.status;
}

namespace {
RemotePluginInstance* remote_of(juce::AudioPluginInstance* instance)
{
    return dynamic_cast<RemotePluginInstance*>(instance);
}
}  // namespace

juce::StringArray PlaybackEngine::take_newly_crashed_plugins()
{
    juce::StringArray names;
    auto note = [&](RemotePluginInstance* r) {
        if (r && r->crashed() && crash_reported_.insert(r).second)
            names.add(r->getName().upToLastOccurrenceOf(" (crashed)", false, false));
    };
    for (auto& [id, slot] : instruments_)
        if (auto* pi = dynamic_cast<PluginInstrument*>(slot.instrument.get())) note(remote_of(&pi->instance()));
    for (auto& [key, proc] : inserts_)
        if (proc) note(remote_of(proc->plugin()));
    return names;
}

int PlaybackEngine::take_late_plugin_blocks()
{
    int late = 0;
    for (auto& [id, slot] : instruments_)
        if (auto* pi = dynamic_cast<PluginInstrument*>(slot.instrument.get()))
            if (auto* r = remote_of(&pi->instance())) late += r->take_late_blocks();
    for (auto& [key, proc] : inserts_)
        if (proc)
            if (auto* r = remote_of(proc->plugin())) late += r->take_late_blocks();
    return late;
}

int PlaybackEngine::reload_crashed_plugins()
{
    int count = 0;
    for (auto& [id, slot] : instruments_)
        if (auto* pi = dynamic_cast<PluginInstrument*>(slot.instrument.get()))
            if (auto* r = remote_of(&pi->instance()); r && r->crashed()) {
                slot.key.clear();  // instrument_for builds a new one from the saved state
                ++count;
            }
    for (auto it = inserts_.begin(); it != inserts_.end();) {
        auto* r = it->second ? remote_of(it->second->plugin()) : nullptr;
        if (r && r->crashed()) {
            it = inserts_.erase(it);  // the snapshot still holding it keeps it alive until retired
            ++count;
        } else {
            ++it;
        }
    }
    if (count > 0) rebuild_now();
    return count;
}

juce::AudioPluginInstance* PlaybackEngine::plugin_for(const std::string& id) const
{
    auto it = instruments_.find(id);
    if (it == instruments_.end()) return nullptr;
    if (auto* p = dynamic_cast<PluginInstrument*>(it->second.instrument.get())) return &p->instance();
    return nullptr;
}

juce::String PlaybackEngine::audio_status(const std::string& track_id) const
{
    auto it = audio_status_.find(track_id);
    return it == audio_status_.end() ? juce::String() : it->second;
}

juce::String PlaybackEngine::insert_status(const std::string& strip_id, int slot) const
{
    auto keys = insert_keys_.find(strip_id);
    if (keys == insert_keys_.end() || slot < 0 || slot >= static_cast<int>(keys->second.size())) return {};
    auto it = inserts_.find(keys->second[static_cast<std::size_t>(slot)]);
    return it == inserts_.end() ? juce::String() : it->second->status();
}

juce::AudioPluginInstance* PlaybackEngine::insert_plugin(const std::string& strip_id, int slot) const
{
    auto keys = insert_keys_.find(strip_id);
    if (keys == insert_keys_.end() || slot < 0 || slot >= static_cast<int>(keys->second.size())) return nullptr;
    auto it = inserts_.find(keys->second[static_cast<std::size_t>(slot)]);
    return it == inserts_.end() ? nullptr : it->second->plugin();
}

void PlaybackEngine::set_render_override(std::optional<RenderOverride> override_settings)
{
    render_override_ = std::move(override_settings);
    rebuild_now();
}

int PlaybackEngine::take_disk_underruns() noexcept { return files_->take_underruns(); }
void PlaybackEngine::set_disk_synchronous(bool on) { files_->set_synchronous(on); }

void PlaybackEngine::set_export_mode(bool on)
{
    if (export_mode_ == on) return;
    export_mode_ = on;
    rebuild_now();
}

float PlaybackEngine::take_peak(const std::string& id)
{
    const StripMeter m = take_meter(id);
    return std::max(m.left, m.right);
}

float PlaybackEngine::take_master_peak() { return take_peak("bus-master"); }

PlaybackEngine::StripMeter PlaybackEngine::take_meter(const std::string& strip_id)
{
    StripMeter m;
    auto it = strips_.find(strip_id);
    if (it == strips_.end()) return m;
    StripDsp& d = *it->second;
    m.left = d.peak_l.exchange(0.0f);
    m.right = d.peak_r.exchange(0.0f);
    m.gain_reduction_db = d.gr_db.exchange(0.0f);
    m.gate_closed = d.gate_closed.exchange(false);
    m.clipped = d.clipped.exchange(false);
    return m;
}

int PlaybackEngine::num_inputs() const { return num_inputs_.load(); }

juce::String PlaybackEngine::input_name(int channel) const
{
    if (auto* device = devices_.getCurrentAudioDevice()) {
        const auto names = device->getInputChannelNames();
        const auto active = device->getActiveInputChannels();
        for (int i = 0, k = 0; i < names.size(); ++i)
            if (active[i] && k++ == channel) return names[i];
    }
    return "In " + juce::String(channel + 1);
}

float PlaybackEngine::take_input_peak(int channel)
{
    if (channel < 0 || channel >= static_cast<int>(input_meters_.size())) return 0.0f;
    return input_meters_[static_cast<std::size_t>(channel)].exchange(0.0f);
}

void PlaybackEngine::set_live_track(const std::string& id)
{
    live_track_ = id;
    request_rebuild();
}

// --- Recording (message thread) -----------------------------------------------------------

void PlaybackEngine::set_record_enabled(bool on)
{
    if (record_enabled_.exchange(on) == on) return;
    if (on) begin_pass();
    else end_passes();
    if (on_transport_changed) on_transport_changed();
}

bool PlaybackEngine::record_enabled() const noexcept { return record_enabled_.load(); }
bool PlaybackEngine::recording() const noexcept { return recording_.load(); }

void PlaybackEngine::set_punch(bool on, Ticks in, Ticks out)
{
    punch_in_ = std::max<Ticks>(0, std::min(in, out));
    punch_out_ = std::max(in, out);
    punch_ = on;
}

bool PlaybackEngine::punch() const noexcept { return punch_.load(); }

double PlaybackEngine::record_seconds() const noexcept
{
    return static_cast<double>(recorded_samples_.load()) / sample_rate_;
}

void PlaybackEngine::report_recording(const juce::String& message, bool ok)
{
    if (on_recording_finished) on_recording_finished(message, ok);
}

void PlaybackEngine::begin_pass()
{
    if (!record_enabled_.load() || !playing_.load()) return;
    if (pass_active_.load() || pending_pass_.load() != nullptr) return;  // one pass at a time
    collect_finished_passes();

    std::vector<const project::Track*> armed;
    for (const project::Track* t : session_.music_tracks())
        if (t->record_armed && t->input >= 0) armed.push_back(t);
    if (armed.empty()) {
        report_recording("No track is record-armed", false);
        return;
    }
    if (device_running_.load() && num_inputs_.load() == 0) {
        report_recording("No audio input device: choose one in Audio Settings to record", false);
        return;
    }
    juce::File folder{juce::String(session_.audio_folder())};
    if (session_.audio_folder().empty())
        folder = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                     .getChildFile("VC Composer 2")
                     .getChildFile("Untitled Audio Files");
    if (!folder.createDirectory()) {
        report_recording("Cannot create the recording folder " + folder.getFullPathName(), false);
        return;
    }
    if (!writer_thread_) {
        writer_thread_ = std::make_unique<juce::TimeSliceThread>("VCC recording writer");
        writer_thread_->startThread(juce::Thread::Priority::high);
    }

    auto pass = std::make_unique<RecordPass>();
    pass->id = next_pass_id_++;
    pass->sample_rate = sample_rate_;
    pass->latency = device_running_.load() ? latency_samples_.load() : 0;
    pass->punch = punch_.load() && punch_out_ > punch_in_;
    const auto& tempo = session_.data().tempo;
    pass->punch_in = std::llround(tempo.seconds_at(punch_in_) * sample_rate_);
    pass->punch_out = std::llround(tempo.seconds_at(punch_out_) * sample_rate_);
    pass->silence.setSize(1, std::max(block_size_, 8192));
    pass->silence.clear();
    // Ten seconds of buffering between the audio thread and the disk.
    const int fifo = static_cast<int>(sample_rate_ * 10.0);
    juce::StringArray problems;
    for (const project::Track* t : armed) {
        RecordTake take;
        take.track_id = t->id;
        take.input = t->input;
        take.channels = t->input_stereo ? 2 : 1;
        take.file = unique_take_file(folder, t->name);
        const juce::String error = open_take(take, sample_rate_, *writer_thread_, fifo);
        if (error.isNotEmpty()) problems.add(juce::String(t->name) + ": " + error);
        else pass->takes.push_back(std::move(take));
    }
    if (pass->takes.empty()) {
        report_recording("Recording failed: " + problems.joinIntoString("; "), false);
        return;
    }
    if (!problems.isEmpty()) report_recording("Not recording " + problems.joinIntoString("; "), false);
    recorded_samples_ = 0;
    pending_pass_.store(pass.release());
}

void PlaybackEngine::end_passes()
{
    stop_passes_upto_.store(next_pass_id_ - 1);
    if (RecordPass* p = pending_pass_.exchange(nullptr)) finalize_pass(std::unique_ptr<RecordPass>(p));
    if (!device_running_.load()) {
        service_passes();  // offline: this thread is the audio thread
    } else {
        // The audio thread hands the pass back within a block or two.
        for (int i = 0; i < 100 && pass_active_.load(); ++i) juce::Thread::sleep(2);
    }
    collect_finished_passes();
}

void PlaybackEngine::collect_finished_passes()
{
    if (RecordPass* p = finished_pass_.exchange(nullptr)) finalize_pass(std::unique_ptr<RecordPass>(p));
}

void PlaybackEngine::finalize_pass(std::unique_ptr<RecordPass> pass)
{
    const juce::File project_dir = session_.path().empty()
                                       ? juce::File()
                                       : juce::File(juce::String(session_.path())).getParentDirectory();
    std::vector<std::pair<std::string, project::AudioClip>> clips;
    juce::StringArray problems;
    double longest = 0.0;
    for (auto& take : pass->takes) {
        take.writer.reset();  // flushes the FIFO and closes the file
        if (take.samples <= 0 || take.start_sample < 0) {
            take.file.deleteFile();
            continue;
        }
        if (take.dropped > 0)
            problems.add(take.file.getFileName() + ": " + juce::String(static_cast<double>(take.dropped) / pass->sample_rate, 2) +
                         " s lost, the disk could not keep up");
        // What actually reached the disk (a full disk truncates the file).
        std::int64_t on_disk = 0;
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(new juce::FileInputStream(take.file), true));
            if (reader) on_disk = reader->lengthInSamples;
        }
        if (on_disk <= 0) {
            problems.add(take.file.getFileName() + " could not be written (disk full?)");
            continue;
        }
        if (on_disk < take.samples) {
            problems.add(take.file.getFileName() + ": only " + juce::String(static_cast<double>(on_disk) / pass->sample_rate, 2) +
                         " s reached the disk (disk full?)");
            take.samples = on_disk;
        }
        project::AudioClip c;
        c.name = take.file.getFileNameWithoutExtension().toStdString();
        c.file = project_dir != juce::File() && take.file.isAChildOf(project_dir)
                     ? take.file.getRelativePathFrom(project_dir).replaceCharacter('\\', '/').toStdString()
                     : take.file.getFullPathName().toStdString();
        // Latency compensation: what was played into the input was heard
        // (input + output latency) earlier than the block it arrived in.
        const double start = static_cast<double>(take.start_sample - pass->latency) / pass->sample_rate;
        c.start_seconds = std::max(0.0, start);
        c.source_offset = std::max(0.0, -start);
        c.length = static_cast<double>(take.samples) / pass->sample_rate - c.source_offset;
        c.channels = take.channels;
        c.file_sample_rate = pass->sample_rate;
        if (c.length <= 0.0) continue;
        longest = std::max(longest, c.length);
        clips.emplace_back(take.track_id, std::move(c));
    }
    recording_ = false;
    if (clips.empty()) {
        if (!problems.isEmpty()) report_recording("Recording failed: " + problems.joinIntoString("; "), false);
        return;
    }
    const auto ids = session_.add_audio_clips(clips, "Record", true);
    if (ids.empty()) {
        report_recording("The recording could not be placed (was the track deleted?)", false);
        return;
    }
    juce::String message = "Recorded " + juce::String(clips.size()) + (clips.size() == 1 ? " track" : " tracks") + " (" +
                           juce::String(longest, 1) + " s)";
    if (!problems.isEmpty()) message << " - " << problems.joinIntoString("; ");
    report_recording(message, problems.isEmpty());
}

// --- Recording (audio thread) ----------------------------------------------------------------

void PlaybackEngine::retire_active_pass() noexcept
{
    if (!active_pass_) return;
    RecordPass* expected = nullptr;
    if (finished_pass_.compare_exchange_strong(expected, active_pass_)) {
        active_pass_ = nullptr;
        pass_active_ = false;
        recording_ = false;
    }
    // else: the message thread has not collected the previous one yet; try again next block.
}

void PlaybackEngine::service_passes() noexcept
{
    const std::uint64_t stop_upto = stop_passes_upto_.load();
    if (active_pass_ && active_pass_->id <= stop_upto) retire_active_pass();
    if (!active_pass_) {
        if (RecordPass* p = pending_pass_.exchange(nullptr)) {
            active_pass_ = p;
            pass_active_ = true;
            if (p->id <= stop_upto) retire_active_pass();
        }
    }
}

// --- Audio thread ---------------------------------------------------------------------------

void PlaybackEngine::audioDeviceIOCallbackWithContext(const float* const* inputs, int num_inputs, float* const* outputs,
                                                      int num_outputs, int num_samples,
                                                      const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < num_outputs; ++ch)
        if (outputs[ch]) juce::FloatVectorOperations::clear(outputs[ch], num_samples);
    process(inputs, num_inputs, outputs, num_outputs, num_samples);
}

void PlaybackEngine::process(const float* const* inputs, int num_inputs, float* const* outputs, int num_outputs,
                             int num_samples) noexcept
{
    if (spill_ && retired_.push(spill_)) spill_ = nullptr;
    if (spill_ == nullptr) {
        if (PlaybackSnapshot* incoming = pending_.exchange(nullptr)) {
            if (current_ && !retired_.push(current_)) spill_ = current_;  // queue full: hand back next block
            current_ = incoming;
            current_->seek(position_samples_.load());
            // Overrides from lanes that are gone are dropped; the live ones
            // are set again before this block is processed.
            current_->for_each_strip([](PlaybackSnapshot::Strip& st) {
                for (auto& ins : st.inserts) ins->clear_automation();
            });
        }
    }
    if (PreviewClip* clip = pending_preview_.exchange(nullptr)) {
        if (preview_) retired_previews_.push(preview_);
        preview_ = clip;
    }
    if (stop_preview_request_.exchange(false) && preview_) {
        preview_->synth->all_notes_off();
        if (retired_previews_.push(preview_)) preview_ = nullptr;
        previewing_ = false;
    }
    service_passes();

    const int max_block = current_ ? current_->max_block : num_samples;
    for (int offset = 0; offset < num_samples; offset += max_block)
        process_chunk(inputs, num_inputs, outputs, num_outputs, offset, std::min(max_block, num_samples - offset));
}

void PlaybackEngine::process_chunk(const float* const* inputs, int num_inputs, float* const* outputs, int num_outputs,
                                   int offset, int n) noexcept
{
    PlaybackSnapshot* s = current_;
    const std::int64_t comp = s ? s->comp : 0;
    const bool stopping = stop_request_.exchange(false);
    const std::int64_t locate = locate_request_.exchange(-1);
    if (stopping && rolling_) {
        // Resume from what was heard, not from the render position ahead of it.
        const std::int64_t heard = std::max(play_start_, position_samples_.load() - comp);
        position_samples_ = heard;
        if (s) s->seek(heard);
    }
    if (s && (stopping || locate >= 0)) {
        for (auto& t : s->tracks)
            if (t.instrument) t.instrument->all_notes_off();
        if (locate >= 0) {
            position_samples_ = locate;
            s->seek(locate);
            // Audio delayed from before the jump is not played after it.
            s->for_each_strip([](PlaybackSnapshot::Strip& st) {
                if (st.out_line) st.out_line->clear();
                for (auto& line : st.send_lines)
                    if (line) line->clear();
            });
        }
    }
    if (locate >= 0) play_start_ = locate;
    if (play_request_.exchange(false)) {
        play_start_ = position_samples_.load();
        if (s) s->seek(play_start_);
    }
    if (locate >= 0 && active_pass_ && active_pass_->recorded > 0) retire_active_pass();  // a jump ends the take
    const bool playing = playing_.load();
    rolling_ = playing;
    const std::int64_t pos = position_samples_.load();
    // The heard timeline: the render position minus the compensation.
    const std::int64_t heard_pos = pos - comp;

    // Input meters.
    for (int ch = 0; ch < num_inputs && ch < static_cast<int>(input_meters_.size()); ++ch)
        if (inputs && inputs[ch]) {
            const auto range = juce::FloatVectorOperations::findMinAndMax(inputs[ch] + offset, n);
            store_max(input_meters_[static_cast<std::size_t>(ch)], std::max(-range.getStart(), range.getEnd()));
        }

    // Recording: inside the punch range when punch is on.
    bool record_range = false;
    // Takes are placed against what the performer heard (the heard
    // timeline); the device latency is taken off when the pass is placed.
    if (RecordPass* pass = active_pass_; pass && playing) {
        std::int64_t a = std::max<std::int64_t>(heard_pos, 0), b = heard_pos + n;
        if (pass->punch) {
            a = std::max(a, pass->punch_in);
            b = std::min(b, pass->punch_out);
        }
        if (b > a) {
            record_range = true;
            pass->write(inputs, num_inputs, offset + static_cast<int>(a - heard_pos), static_cast<int>(b - a), a);
            recorded_samples_.store(pass->recorded, std::memory_order_relaxed);
        }
        recording_.store(record_range, std::memory_order_relaxed);
        if (pass->punch && heard_pos + n >= pass->punch_out) retire_active_pass();
    }

    if (s && n <= s->max_block) {
        s->out.clear(0, n);
        for (auto& b : s->buses) b.buffer.clear(0, n);
        for (std::size_t i = 0; i < s->tracks.size(); ++i) {
            auto& t = s->tracks[i];
            juce::AudioBuffer<float> buf(t.buffer.getArrayOfWritePointers(), 2, n);
            buf.clear();
            automate_strip(t.strip, pos, n, playing);
            if (t.instrument) {
                t.midi.clear();
                if (playing) gather(t.events, t.cursor, pos, n, t.midi);
                if (static_cast<int>(i) == s->live_track) live_input_.removeNextBlockOfMessages(t.midi, n);
                t.instrument->process(buf, t.midi, n);
            }
            // Tape-style auto input: an armed track hears its input while
            // stopped or recording, and its clips otherwise.
            const bool monitor_input = t.monitor == 2 || (t.monitor == 1 && t.armed && (!playing || record_range));
            float* l = buf.getWritePointer(0);
            float* r = buf.getWritePointer(1);
            if (playing && !monitor_input)
                for (const auto& clip : t.clips) render_clip(clip, l, r, pos, n, t.mono);
            else if (!playing)
                for (const auto& clip : t.clips) cue_clip(clip, pos);
            if (monitor_input && inputs && t.input >= 0 && t.input < num_inputs && inputs[t.input]) {
                const float* in_l = inputs[t.input] + offset;
                const int right = t.input_stereo && t.input + 1 < num_inputs && inputs[t.input + 1] ? t.input + 1 : t.input;
                const float* in_r = inputs[right] + offset;
                juce::FloatVectorOperations::add(l, in_l, n);
                juce::FloatVectorOperations::add(r, in_r, n);
            }
            // Low-latency monitoring: what the performer plays skips the compensation.
            const bool low_latency = monitor_input || (static_cast<int>(i) == s->live_track && (!playing || t.armed));
            process_strip(*s, t.strip, buf, n, playing, low_latency);
        }
        for (int bi : s->bus_order) {
            auto& b = s->buses[static_cast<std::size_t>(bi)];
            juce::AudioBuffer<float> buf(b.buffer.getArrayOfWritePointers(), 2, n);
            automate_strip(b.strip, pos, n, playing);
            process_strip(*s, b.strip, buf, n, playing, false);
        }
        if (playing && metronome_.load()) {
            // Scheduled on the heard timeline, so the click lines up with the
            // compensated mix.
            while (s->click_cursor < s->clicks.size() && s->clicks[s->click_cursor].first < heard_pos + n) {
                const auto click = s->clicks[s->click_cursor++];
                if (click.first < heard_pos) continue;
                const int at = static_cast<int>(click.first - heard_pos);
                const double f = click.second ? 1760.0 : 1320.0;
                const int len = std::min(n - at, static_cast<int>(s->sample_rate * 0.03));
                for (int k = 0; k < len; ++k) {
                    const float v = 0.25f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * f * k / s->sample_rate) *
                                                               (1.0 - static_cast<double>(k) / len));
                    s->out.addSample(0, at + k, v);
                    s->out.addSample(1, at + k, v);
                }
            }
        } else if (s) {
            // Keep the click cursor in step while it is off.
            while (s->click_cursor < s->clicks.size() && s->clicks[s->click_cursor].first < heard_pos + n) ++s->click_cursor;
        }
        for (int ch = 0; ch < num_outputs && ch < 2; ++ch)
            if (outputs[ch]) juce::FloatVectorOperations::add(outputs[ch] + offset, s->out.getReadPointer(ch), n);
    }

    if (PreviewClip* c = preview_; c && n <= c->buffer.getNumSamples()) {
        c->midi.clear();
        std::int64_t from = c->position;
        if (c->loop) {
            int done = 0;
            while (done < n) {
                const int chunk = static_cast<int>(std::min<std::int64_t>(n - done, c->length - from));
                const std::int64_t to = from + chunk;
                while (c->cursor < c->events.size() && c->events[c->cursor].sample < to) {
                    const Event& e = c->events[c->cursor++];
                    if (e.sample >= from) c->midi.addEvent(e.bytes, e.size, done + static_cast<int>(e.sample - from));
                }
                done += chunk;
                from = to;
                if (from >= c->length) {
                    // Close every note before wrapping so nothing hangs.
                    for (int ch = 1; ch <= 16; ++ch)
                        c->midi.addEvent(juce::MidiMessage::allNotesOff(ch), std::max(0, done - 1));
                    from = 0;
                    c->cursor = 0;
                }
            }
        } else {
            gather(c->events, c->cursor, from, n, c->midi);
            from += n;
        }
        c->position = from;
        juce::AudioBuffer<float> buf(c->buffer.getArrayOfWritePointers(), 2, n);
        buf.clear();
        c->synth->process(buf, c->midi, n);
        for (int ch = 0; ch < num_outputs && ch < 2; ++ch)
            if (outputs[ch]) juce::FloatVectorOperations::add(outputs[ch] + offset, buf.getReadPointer(ch), n);
        // A one-shot audition ends after its release tail has rung out.
        const auto tail = static_cast<std::int64_t>(2.5 * sample_rate_);
        if (!c->loop && c->position >= c->length + tail && retired_previews_.push(c)) {
            preview_ = nullptr;
            previewing_ = false;
        }
    }

    if (playing) position_samples_.store(pos + n);
    const std::int64_t heard = playing ? std::max(play_start_, pos + n - comp) : position_samples_.load();
    heard_samples_.store(heard, std::memory_order_relaxed);
    gestures_->heard.store(heard, std::memory_order_relaxed);
}

// --- Automation (message thread) ---------------------------------------------------------

namespace {
std::string slot_key(const std::string& owner, const std::string& param) { return owner + '\x1f' + param; }
bool writing_mode(const std::string& m) { return m == "touch" || m == "latch" || m == "write"; }
}  // namespace

std::shared_ptr<AutoSlot> PlaybackEngine::slot_for(const std::string& owner, const std::string& param, bool* created)
{
    auto& slot = auto_slots_[slot_key(owner, param)];
    if (created) *created = !slot;
    if (!slot) slot = std::make_shared<AutoSlot>();
    return slot;
}

std::string PlaybackEngine::mode_of(const std::string& owner) const
{
    const auto& d = session_.data();
    if (const auto* t = d.find_track(owner)) return t->automation_mode;
    if (const auto* b = d.find_bus(owner)) return b->automation_mode;
    return "off";
}

std::optional<double> PlaybackEngine::static_of(const std::string& owner, const std::string& param) const
{
    const auto& d = session_.data();
    // Plug-in parameters live in the plug-in: its current value.
    juce::AudioPluginInstance* instance = nullptr;
    int index = -1;
    if (param.rfind("plugin:", 0) == 0) {
        instance = plugin_for(owner);
        index = std::atoi(param.c_str() + 7);
    } else if (param.rfind("insert_plugin:", 0) == 0) {
        const auto colon = param.find(':', 14);
        if (colon != std::string::npos) {
            instance = insert_plugin(owner, std::atoi(param.substr(14, colon - 14).c_str()));
            index = std::atoi(param.c_str() + colon + 1);
        }
    }
    if (instance) {
        if (index < 0 || index >= instance->getParameters().size()) return std::nullopt;
        return static_cast<double>(instance->getParameters()[index]->getValue());
    }
    if (const auto* t = d.find_track(owner)) return project::static_value(*t, param);
    if (const auto* b = d.find_bus(owner)) return project::static_value(*b, param);
    return std::nullopt;
}

std::string PlaybackEngine::owner_name(const std::string& owner) const
{
    const auto& d = session_.data();
    if (const auto* t = d.find_track(owner)) return t->name;
    if (const auto* b = d.find_bus(owner)) return b->name;
    return owner;
}

std::string PlaybackEngine::param_name(const std::string& owner, const std::string& param) const
{
    // A plug-in parameter: the plug-in's own name for it.
    const auto colon = param.rfind(':');
    juce::AudioPluginInstance* instance = nullptr;
    if (param.rfind("plugin:", 0) == 0) instance = plugin_for(owner);
    else if (param.rfind("insert_plugin:", 0) == 0 && colon > 14)
        instance = insert_plugin(owner, std::atoi(param.substr(14, colon - 14).c_str()));
    if (instance && colon != std::string::npos) {
        const int index = std::atoi(param.c_str() + colon + 1);
        if (index >= 0 && index < instance->getParameters().size())
            return instance->getParameters()[index]->getName(64).toStdString();
    }
    if (const auto* lanes = session_.lanes_of(owner))
        if (const auto* lane = project::find_lane(*lanes, param); lane && !lane->name.empty()) return lane->name;
    const auto& d = session_.data();
    std::vector<project::ParamInfo> params;
    if (const auto* t = d.find_track(owner)) params = project::automatable_params(*t, d);
    else if (const auto* b = d.find_bus(owner)) params = project::automatable_params(*b, d);
    for (const auto& p : params)
        if (p.id == param) return p.name;
    const auto info = project::param_info(param);
    return info ? info->name : param;
}

Ticks PlaybackEngine::tick_at_sample(std::int64_t sample) const
{
    return session_.data().tempo.tick_at_seconds(static_cast<double>(std::max<std::int64_t>(0, sample)) / sample_rate_);
}

/// Appends a written value. The previous value is held up to it (what was
/// heard); write_pass thins the hold points away where a control moved smoothly.
void PlaybackEngine::add_point(WriteSegment& seg, Ticks tick, double value)
{
    if (!seg.points.empty() && seg.points.back().tick < tick - 1) seg.points.push_back({tick - 1, seg.value, "linear"});
    if (!seg.points.empty() && seg.points.back().tick >= tick) seg.points.back().value = value;
    else seg.points.push_back({tick, value, "linear"});
    seg.value = value;
}

PlaybackEngine::WriteSegment* PlaybackEngine::find_segment(const std::string& key)
{
    for (auto& seg : segments_)
        if (seg.key == key) return &seg;
    return nullptr;
}

bool PlaybackEngine::open_segment(const std::string& owner, const std::string& param, double value, bool held,
                                  bool from_static, std::int64_t at)
{
    if (!project::param_info(param)) return false;
    bool created = false;
    WriteSegment seg;
    seg.key = slot_key(owner, param);
    seg.owner = owner;
    seg.param = param;
    seg.name = param_name(owner, param);
    // The pass starts a tick before the first written value, so the lane's
    // own value is kept up to the moment the control was grabbed.
    const Ticks tick = tick_at_sample(at);
    seg.from = std::max<Ticks>(0, tick - 1);
    seg.value = value;
    seg.held = held;
    seg.from_static = from_static;
    seg.slot = slot_for(owner, param, &created);
    seg.slot->value.store(value);
    seg.slot->mode.store(from_static ? 2 : 1);
    seg.points.push_back({tick, value, "linear"});
    segments_.push_back(std::move(seg));
    return created;  // a parameter the snapshot does not automate yet
}

void PlaybackEngine::close_segment(std::size_t index, std::int64_t at)
{
    WriteSegment seg = std::move(segments_[index]);
    segments_.erase(segments_.begin() + static_cast<std::ptrdiff_t>(index));
    Ticks to = std::max(seg.from, tick_at_sample(at));
    add_point(seg, to, seg.from_static ? static_of(seg.owner, seg.param).value_or(seg.value) : seg.value);
    seg.slot->mode.store(0);  // back to the lane
    session::Session::AutomationWrite w;
    w.owner_id = seg.owner;
    w.param = seg.param;
    w.name = seg.name;
    w.from = seg.from;
    w.to = to;
    w.points = std::move(seg.points);
    written_.push_back(std::move(w));
}

void PlaybackEngine::begin_touch(const std::string& owner, const std::string& param)
{
    begin_touch_at(owner, param, heard_samples_.load());
}

void PlaybackEngine::touch_value(const std::string& owner, const std::string& param, double value)
{
    touch_value_at(owner, param, value, heard_samples_.load());
}

void PlaybackEngine::end_touch(const std::string& owner, const std::string& param)
{
    end_touch_at(owner, param, heard_samples_.load());
}

void PlaybackEngine::begin_touch_at(const std::string& owner, const std::string& param, std::int64_t at)
{
    const std::string key = slot_key(owner, param);
    touched_.insert(key);
    if (!writes_automation(owner, param)) return;
    if (WriteSegment* seg = find_segment(key)) {
        seg->held = true;
        if (seg->from_static) {
            // Write mode: the static value was being written; the touch takes over.
            seg->from_static = false;
            if (const auto v = static_of(owner, param)) add_point(*seg, tick_at_sample(at), *v);
            seg->slot->value.store(seg->value);
            seg->slot->mode.store(1);
        }
        return;
    }
    // Starts from what is playing now, so grabbing a control does not jump.
    const double v = automated_value(owner, param).value_or(static_of(owner, param).value_or(
        project::param_info(param) ? project::param_info(param)->default_value : 0.0));
    if (open_segment(owner, param, v, true, false, at)) rebuild_now();
}

void PlaybackEngine::touch_value_at(const std::string& owner, const std::string& param, double value, std::int64_t at)
{
    WriteSegment* seg = find_segment(slot_key(owner, param));
    if (!seg) {
        // A value without a begin_touch (a click on a switch): a short gesture.
        if (!writes_automation(owner, param)) return;
        begin_touch_at(owner, param, at);
        seg = find_segment(slot_key(owner, param));
        if (!seg) return;
        touched_.erase(seg->key);
        seg->held = false;
    }
    if (const auto info = project::param_info(param)) value = std::clamp(value, info->min, info->max);
    const Ticks tick = tick_at_sample(at);
    add_point(*seg, tick, value);
    seg->from_static = false;
    seg->slot->value.store(value);
    seg->slot->mode.store(1);
    if (!seg->held && !touched_.count(seg->key) && mode_of(owner) == "touch") {
        // A one-off value in touch mode is written as a short step.
        const auto index = static_cast<std::size_t>(seg - segments_.data());
        close_segment(index, at);
    }
}

void PlaybackEngine::end_touch_at(const std::string& owner, const std::string& param, std::int64_t at)
{
    const std::string key = slot_key(owner, param);
    touched_.erase(key);
    WriteSegment* seg = find_segment(key);
    if (!seg) return;
    seg->held = false;
    // Touch returns to the lane on release; latch and write keep the value.
    if (mode_of(owner) == "touch") close_segment(static_cast<std::size_t>(seg - segments_.data()), at);
}

void PlaybackEngine::start_automation_pass()
{
    writing_pass_ = true;
    refresh_writes();
}

void PlaybackEngine::refresh_writes()
{
    if (!writing_pass_ || !playing_.load()) return;
    const std::int64_t at = heard_samples_.load();
    bool rebuild = false;
    // Owners that left the writing modes stop writing.
    for (std::size_t i = segments_.size(); i-- > 0;) {
        const std::string mode = mode_of(segments_[i].owner);
        if (!writing_mode(mode) || (mode == "touch" && !segments_[i].held)) close_segment(i, at);
    }
    auto owner = [&](const std::string& id, const std::string& mode, const std::vector<project::AutomationLane>& lanes) {
        if (!writing_mode(mode)) return;
        // Controls already held when the pass (re)starts.
        for (const auto& key : touched_)
            if (key.rfind(id + '\x1f', 0) == 0 && !find_segment(key)) {
                const std::string param = key.substr(id.size() + 1);
                const double v = static_of(id, param).value_or(0.0);
                rebuild = open_segment(id, param, v, true, false, at) || rebuild;
            }
        if (mode != "write") return;
        // Write: every lane of the strip, with the control's current (static) value.
        for (const auto& lane : lanes) {
            if (!lane.enabled) continue;
            WriteSegment* seg = find_segment(slot_key(id, lane.param));
            const auto v = static_of(id, lane.param);
            if (!seg) {
                if (v) rebuild = open_segment(id, lane.param, *v, false, true, at) || rebuild;
            } else if (seg->from_static && v && !juce::exactlyEqual(*v, seg->value)) {
                // The static value moved during the pass (a console edit): write it.
                add_point(*seg, tick_at_sample(at), *v);
            }
        }
    };
    for (const auto& t : session_.data().tracks) owner(t.id, t.automation_mode, t.automation);
    for (const auto& b : session_.data().buses) owner(b.id, b.automation_mode, b.automation);
    if (rebuild) rebuild_now();
}

void PlaybackEngine::finish_automation_pass()
{
    drain_gestures();
    const std::int64_t at = heard_samples_.load();
    while (!segments_.empty()) close_segment(segments_.size() - 1, at);
    writing_pass_ = false;
    if (written_.empty()) return;
    std::vector<session::Session::AutomationWrite> writes;
    juce::StringArray names;
    for (auto& w : written_) {
        if (session_.lanes_of(w.owner_id) == nullptr) continue;  // the strip was deleted
        names.addIfNotAlreadyThere(juce::String(param_name(w.owner_id, w.param)) + " (" + juce::String(owner_name(w.owner_id)) + ")");
        writes.push_back(std::move(w));
    }
    written_.clear();
    if (writes.empty()) return;
    committing_ = true;
    const bool ok = session_.commit_automation_pass(writes);
    committing_ = false;
    if (ok && on_automation_written) on_automation_written("Wrote automation: " + names.joinIntoString(", "));
}

std::optional<double> PlaybackEngine::automated_value(const std::string& owner, const std::string& param) const
{
    for (const auto& seg : segments_)
        if (seg.owner == owner && seg.param == param) return seg.from_static ? static_of(owner, param) : seg.value;
    if (mode_of(owner) == "off") return std::nullopt;
    const auto* lanes = session_.lanes_of(owner);
    const auto* lane = lanes ? project::find_lane(*lanes, param) : nullptr;
    if (!lane || !lane->enabled || lane->points.empty()) return std::nullopt;
    return project::value_at(*lane, position());
}

bool PlaybackEngine::writes_automation(const std::string& owner, const std::string& param) const
{
    return playing_.load() && writing_mode(mode_of(owner)) && project::param_info(param).has_value();
}

void PlaybackEngine::watch_plugin(juce::AudioPluginInstance& instance, const void* holder, bool has_watch,
                                  const std::function<void(std::unique_ptr<ParameterWatch>)>& attach,
                                  std::map<int, std::pair<std::string, std::string>>& targets,
                                  const std::string& owner, const std::string& prefix)
{
    int& id = watch_ids_[holder];
    if (!has_watch || id == 0) {
        id = next_watch_++;
        attach(std::make_unique<ParameterWatch>(instance, gestures_, id));
    }
    targets[id] = {owner, prefix};
}

void PlaybackEngine::drain_gestures()
{
    GestureEvent e;
    while (gestures_->pop(e)) {
        auto it = insert_watch_targets_.find(e.watch);
        if (it == insert_watch_targets_.end()) {
            it = instrument_watch_targets_.find(e.watch);
            if (it == instrument_watch_targets_.end()) continue;
        }
        const std::string owner = it->second.first;
        const std::string param = it->second.second + std::to_string(e.param);
        const bool held = touched_.count(slot_key(owner, param)) > 0;
        if (e.kind == 1) begin_touch_at(owner, param, e.at);
        else if (e.kind == 2) end_touch_at(owner, param, e.at);
        else if (held) touch_value_at(owner, param, e.value, e.at);  // values outside a gesture are not ours
    }
}

std::vector<int> PlaybackEngine::current_latencies() const
{
    std::vector<int> out;
    for (const auto& [id, slot] : instruments_) out.push_back(slot.instrument ? slot.instrument->latency_samples() : 0);
    for (const auto& [key, insert] : inserts_) out.push_back(insert ? insert->latency_samples() : 0);
    return out;
}

// --- Plug-in delay compensation ---------------------------------------------------------------

void PlaybackEngine::set_delay_compensation(bool on)
{
    if (pdc_ == on) return;
    pdc_ = on;
    rebuild_now();
}

bool PlaybackEngine::delay_compensation() const noexcept { return pdc_; }

int PlaybackEngine::latency_samples(const std::string& strip_id) const
{
    auto it = strip_latency_.find(strip_id);
    return it == strip_latency_.end() ? 0 : it->second;
}

int PlaybackEngine::compensation_samples() const { return compensation_; }

}  // namespace vcc::desktop
