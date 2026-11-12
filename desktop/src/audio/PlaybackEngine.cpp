#include "audio/PlaybackEngine.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "audio/AudioFileCache.h"
#include "audio/ChannelDsp.h"
#include "audio/PlaceholderSynth.h"
#include "audio/Recorder.h"
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

/// Every mix setting of one strip, computed on the message thread and sent
/// to the audio thread through a Mailbox. Gains are final (fader, pan law,
/// mute/solo folded in), so the audio thread only ramps to them.
struct StripParams {
    project::ChannelStrip strip;
    float out_l = 0.0f, out_r = 0.0f;
    std::array<float, kMaxSends> send_l{}, send_r{};
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
    // Meters (audio thread writes, message thread takes).
    std::atomic<float> peak_l{0.0f}, peak_r{0.0f}, gr_db{0.0f};
    std::atomic<bool> gate_closed{false}, clipped{false};
};

struct PlaybackSnapshot {
    double sample_rate = 48000.0;
    int max_block = 512;

    struct Strip {
        std::shared_ptr<StripDsp> dsp;
        std::vector<std::shared_ptr<InsertProcessor>> inserts;
        std::vector<int> sends;  // destination bus per send (-1 = none), aligned with StripParams
        int output = -1;         // bus index; -1 = the device (master bus only)
    };
    struct Clip {
        std::shared_ptr<const AudioData> data;
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
        click_cursor = static_cast<std::size_t>(
            std::lower_bound(clicks.begin(), clicks.end(), sample,
                             [](const std::pair<std::int64_t, bool>& c, std::int64_t s) { return c.first < s; }) -
            clicks.begin());
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
};

std::vector<const project::Track*> played_tracks(const session::Session& session, bool export_mode)
{
    return export_mode ? session.project().export_tracks() : session.music_tracks();
}

MixPlan make_plan(const project::ProjectData& d, std::vector<const project::Track*> tracks)
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

    // Solo-in-place.
    bool any_solo = false;
    for (const auto* t : p.tracks) any_solo = any_solo || t->solo;
    for (int i = 0; i < n; ++i)
        if (const auto* b = p.buses[static_cast<std::size_t>(i)]; b && i != p.master) any_solo = any_solo || b->solo;
    std::vector<char> down(static_cast<std::size_t>(n), 0), soloed_bus(static_cast<std::size_t>(n), 0);
    std::vector<int> stack;
    for (std::size_t k = 0; k < p.tracks.size(); ++k)
        if (p.tracks[k]->solo) {
            stack.push_back(p.track_output[k]);
            for (int dest : p.track_sends[k])
                if (dest >= 0) stack.push_back(dest);
        }
    for (int i = 0; i < n; ++i)
        if (const auto* b = p.buses[static_cast<std::size_t>(i)]; b && b->solo) {
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
        p.track_audible.push_back(!t->mute && (!any_solo || t->solo || feeds_soloed_bus(p.track_output[k])));
    }
    for (int i = 0; i < n; ++i) {
        const auto* b = p.buses[static_cast<std::size_t>(i)];
        if (!b) {
            p.bus_audible.push_back(true);
            continue;
        }
        const bool safe = i == p.master || b->kind == "fx";
        p.bus_audible.push_back(!b->mute && (!any_solo || safe || b->solo || down[static_cast<std::size_t>(i)] ||
                                             feeds_soloed_bus(p.bus_output[static_cast<std::size_t>(i)])));
    }
    return p;
}

StripParams make_params(const project::ChannelStrip& strip, double gain_db, double pan, bool audible, bool mono,
                        const std::vector<project::Send>& sends)
{
    StripParams p;
    p.strip = strip;
    const float fader = audible ? db_to_gain(gain_db) : 0.0f;
    const auto [pl, pr] = pan_gains(pan, mono);
    p.out_l = fader * pl;
    p.out_r = fader * pr;
    for (std::size_t i = 0; i < sends.size() && i < static_cast<std::size_t>(kMaxSends); ++i) {
        const auto& s = sends[i];
        if (s.mute || !audible) continue;
        const float level = db_to_gain(s.level_db) * (s.pre_fader ? 1.0f : fader);
        const auto [sl, sr] = pan_gains(s.pan_follow ? pan : s.pan, mono);
        p.send_l[i] = level * sl;
        p.send_r[i] = level * sr;
    }
    return p;
}

// --- Audio thread helpers ---------------------------------------------------------------

void render_clip(const PlaybackSnapshot::Clip& c, float* l, float* r, std::int64_t pos, int n, bool mono) noexcept
{
    const std::int64_t a = std::max(pos, c.start), b = std::min(pos + n, c.start + c.length);
    if (b <= a) return;
    const AudioData& d = *c.data;
    const std::int64_t available = d.length();
    const float* src_l = d.samples.getReadPointer(0);
    const bool stereo = d.channels() > 1;
    const float* src_r = stereo ? d.samples.getReadPointer(1) : src_l;
    constexpr float half_pi = juce::MathConstants<float>::halfPi;
    for (std::int64_t t = a; t < b; ++t) {
        const std::int64_t k = t - c.start;
        const std::int64_t src = c.offset + k;
        if (src < 0) continue;
        if (src >= available) break;
        const std::int64_t remaining = c.length - 1 - k;
        float g = c.gain;
        if (k < c.fade_in) g *= std::sin(half_pi * (static_cast<float>(k) + 0.5f) / static_cast<float>(c.fade_in));
        if (remaining < c.fade_out)
            g *= std::sin(half_pi * (static_cast<float>(remaining) + 0.5f) / static_cast<float>(c.fade_out));
        if (k < c.declick) g *= (static_cast<float>(k) + 0.5f) / static_cast<float>(c.declick);
        if (remaining < c.declick) g *= (static_cast<float>(remaining) + 0.5f) / static_cast<float>(c.declick);
        const auto idx = static_cast<std::size_t>(src);
        float vl = src_l[idx], vr = src_r[idx];
        if (mono && stereo) vl = vr = 0.5f * (vl + vr);
        const auto i = static_cast<std::size_t>(t - pos);
        l[i] += vl * g;
        r[i] += vr * g;
    }
}

/// Channel strip, inserts, sends, fader/pan and meters; adds the result to
/// the strip's output (a bus, or the device for the master).
void process_strip(PlaybackSnapshot& s, PlaybackSnapshot::Strip& strip, juce::AudioBuffer<float>& buf, int n) noexcept
{
    StripDsp& d = *strip.dsp;
    const StripParams& p = d.params.latest();
    float* l = buf.getWritePointer(0);
    float* r = buf.getWritePointer(1);
    d.channel.process(l, r, n, p.strip);
    for (const auto& ins : strip.inserts)
        if (!ins->bypass.load(std::memory_order_relaxed)) ins->process(l, r, n);
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
        if (d.cur_send_l[i] > 0.0f || p.send_l[i] > 0.0f) target.addFromWithRamp(0, 0, l, n, d.cur_send_l[i], p.send_l[i]);
        if (d.cur_send_r[i] > 0.0f || p.send_r[i] > 0.0f) target.addFromWithRamp(1, 0, r, n, d.cur_send_r[i], p.send_r[i]);
        d.cur_send_l[i] = p.send_l[i];
        d.cur_send_r[i] = p.send_r[i];
    }
    buf.applyGainRamp(0, 0, n, d.cur_l, p.out_l);
    buf.applyGainRamp(1, 0, n, d.cur_r, p.out_r);
    d.cur_l = p.out_l;
    d.cur_r = p.out_r;

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
    : session_(session), plugins_(plugins), files_(std::make_unique<AudioFileCache>())
{
    for (auto& m : input_meters_) m.store(0.0f);
    listener_ = session_.add_listener([this](session::Change c) {
        if (c != session::Change::Document) return;
        sync_processors();  // mix changes are heard at once; structure follows with the rebuild
        request_rebuild();
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
    files_->keep_only_rate(sample_rate_);
}

void PlaybackEngine::request_rebuild() { rebuild_pending_ = true; }

void PlaybackEngine::timerCallback()
{
    if (rebuild_pending_) rebuild_now();
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
    const MixPlan plan = make_plan(d, played_tracks(session_, export_mode_));
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
            keys[i] = key;
            live_inserts.insert(key);
        }
    };

    for (std::size_t k = 0; k < plan.tracks.size(); ++k) {
        const project::Track& t = *plan.tracks[k];
        strip_for(t.id).params.write(make_params(t.strip, t.gain_db, t.pan, plan.track_audible[k], is_mono_track(t), t.sends));
        sync_inserts(t.id, t.inserts);
    }
    for (std::size_t i = 0; i < plan.buses.size(); ++i) {
        const project::Bus* b = plan.buses[i];
        StripDsp& strip = strip_for(plan.bus_ids[i]);
        if (!b) {
            strip.params.write(make_params({}, 0.0, 0.0, true, false, {}));
            continue;
        }
        strip.params.write(make_params(b->strip, b->gain_db, b->pan, plan.bus_audible[i], false, b->sends));
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
    const MixPlan plan = make_plan(d, played_tracks(session_, export_mode_));
    auto snap = std::make_unique<PlaybackSnapshot>();
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
            project::RenderedTrack r = project::render_track(*t);
            if (st.instrument->wants_keyswitches()) r.notes.insert(r.notes.end(), r.keyswitches.begin(), r.keyswitches.end());
            st.events = to_events(r.notes, r.controllers, d.tempo, sample_rate_);
            st.midi.ensureSize(4096);
        }
        if (const project::TrackVersion* v = t->active_version()) {
            juce::StringArray problems;
            for (const auto& c : v->audio_clips) {
                if (c.lane != 0 || c.muted || c.length <= 0.0) continue;
                juce::String error;
                auto data = files_->get(session_.resolve_audio_path(c.file), sample_rate_, error);
                if (!data) {
                    problems.addIfNotAlreadyThere(error);
                    continue;
                }
                PlaybackSnapshot::Clip clip;
                clip.data = std::move(data);
                clip.start = std::llround(c.start_seconds * sample_rate_);
                clip.length = std::llround(c.length * sample_rate_);
                clip.offset = std::llround(std::max(0.0, c.source_offset) * sample_rate_);
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
    play_request_ = true;
    playing_ = true;
    begin_pass();
    if (on_transport_changed) on_transport_changed();
}

void PlaybackEngine::stop()
{
    stop_request_ = true;
    playing_ = false;
    end_passes();
    if (on_transport_changed) on_transport_changed();
}

void PlaybackEngine::toggle_play() { playing() ? stop() : play(); }

void PlaybackEngine::locate(Ticks tick)
{
    const auto sample = static_cast<std::int64_t>(std::llround(session_.data().tempo.seconds_at(std::max<Ticks>(0, tick)) * sample_rate_));
    locate_request_ = sample;
    position_samples_ = sample;  // the UI sees the new position immediately
    if (on_transport_changed) on_transport_changed();
}

double PlaybackEngine::position_seconds() const { return static_cast<double>(position_samples_.load()) / sample_rate_; }

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
    return it == instruments_.end() ? juce::String("No instrument") : it->second.status;
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
        files_->forget(take.file.getFullPathName().toStdString());
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
    const bool stopping = stop_request_.exchange(false);
    const std::int64_t locate = locate_request_.exchange(-1);
    if (play_request_.exchange(false) && s) s->seek(position_samples_.load());
    if (s && (stopping || locate >= 0)) {
        for (auto& t : s->tracks)
            if (t.instrument) t.instrument->all_notes_off();
        if (locate >= 0) {
            position_samples_ = locate;
            s->seek(locate);
        }
    }
    if (locate >= 0 && active_pass_ && active_pass_->recorded > 0) retire_active_pass();  // a jump ends the take
    const bool playing = playing_.load();
    const std::int64_t pos = position_samples_.load();

    // Input meters.
    for (int ch = 0; ch < num_inputs && ch < static_cast<int>(input_meters_.size()); ++ch)
        if (inputs && inputs[ch]) {
            const auto range = juce::FloatVectorOperations::findMinAndMax(inputs[ch] + offset, n);
            store_max(input_meters_[static_cast<std::size_t>(ch)], std::max(-range.getStart(), range.getEnd()));
        }

    // Recording: inside the punch range when punch is on.
    bool record_range = false;
    if (RecordPass* pass = active_pass_; pass && playing) {
        std::int64_t a = pos, b = pos + n;
        if (pass->punch) {
            a = std::max(a, pass->punch_in);
            b = std::min(b, pass->punch_out);
        }
        if (b > a) {
            record_range = true;
            pass->write(inputs, num_inputs, offset + static_cast<int>(a - pos), static_cast<int>(b - a), a);
            recorded_samples_.store(pass->recorded, std::memory_order_relaxed);
        }
        recording_.store(record_range, std::memory_order_relaxed);
        if (pass->punch && pos + n >= pass->punch_out) retire_active_pass();
    }

    if (s && n <= s->max_block) {
        s->out.clear(0, n);
        for (auto& b : s->buses) b.buffer.clear(0, n);
        for (std::size_t i = 0; i < s->tracks.size(); ++i) {
            auto& t = s->tracks[i];
            juce::AudioBuffer<float> buf(t.buffer.getArrayOfWritePointers(), 2, n);
            buf.clear();
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
            if (monitor_input && inputs && t.input >= 0 && t.input < num_inputs && inputs[t.input]) {
                const float* in_l = inputs[t.input] + offset;
                const int right = t.input_stereo && t.input + 1 < num_inputs && inputs[t.input + 1] ? t.input + 1 : t.input;
                const float* in_r = inputs[right] + offset;
                juce::FloatVectorOperations::add(l, in_l, n);
                juce::FloatVectorOperations::add(r, in_r, n);
            }
            process_strip(*s, t.strip, buf, n);
        }
        for (int bi : s->bus_order) {
            auto& b = s->buses[static_cast<std::size_t>(bi)];
            juce::AudioBuffer<float> buf(b.buffer.getArrayOfWritePointers(), 2, n);
            process_strip(*s, b.strip, buf, n);
        }
        if (playing && metronome_.load()) {
            while (s->click_cursor < s->clicks.size() && s->clicks[s->click_cursor].first < pos + n) {
                const auto click = s->clicks[s->click_cursor++];
                if (click.first < pos) continue;
                const int at = static_cast<int>(click.first - pos);
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
            while (s->click_cursor < s->clicks.size() && s->clicks[s->click_cursor].first < pos + n) ++s->click_cursor;
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
}

}  // namespace vcc::desktop
