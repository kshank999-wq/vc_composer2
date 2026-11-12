#include "audio/PlaybackEngine.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "audio/PlaceholderSynth.h"
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

}  // namespace

struct PlaybackSnapshot {
    double sample_rate = 48000.0;
    int max_block = 512;

    struct Track {
        std::shared_ptr<Instrument> instrument;
        std::vector<Event> events;
        std::size_t cursor = 0;  // audio thread
        float gain_left = 1.0f, gain_right = 1.0f;
        bool audible = true;
        int output_bus = -1;  // -1 = master
        int meter = 0;
        juce::AudioBuffer<float> buffer;
        juce::MidiBuffer midi;
    };
    struct Bus {
        float gain = 1.0f;
        int output = -1;
        std::vector<std::pair<int, float>> sends;
        std::unique_ptr<juce::Reverb> reverb;
        juce::AudioBuffer<float> buffer;
    };
    std::vector<Track> tracks;
    std::vector<Bus> buses;
    std::vector<int> bus_order;  // deepest first
    std::vector<std::pair<std::int64_t, bool>> clicks;  // sample, downbeat
    std::size_t click_cursor = 0;
    juce::AudioBuffer<float> master;
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

void add_peak(std::atomic<float>& meter, const juce::AudioBuffer<float>& b, int n) noexcept
{
    float peak = 0.0f;
    for (int ch = 0; ch < b.getNumChannels(); ++ch) peak = std::max(peak, b.getMagnitude(ch, 0, n));
    float old = meter.load(std::memory_order_relaxed);
    while (peak > old && !meter.compare_exchange_weak(old, peak, std::memory_order_relaxed)) {}
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

PlaybackEngine::PlaybackEngine(session::Session& session, PluginHost& plugins) : session_(session), plugins_(plugins)
{
    for (auto& m : meters_) m.store(0.0f);
    listener_ = session_.add_listener([this](session::Change c) {
        if (c == session::Change::Document) request_rebuild();
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
    juce::String error = devices_.initialiseWithDefaultDevices(0, 2);
    if (error.isEmpty() && devices_.getCurrentAudioDevice() == nullptr) error = "no audio output device was found";
    if (error.isEmpty()) devices_.addAudioCallback(this);
    for (const auto& input : juce::MidiInput::getAvailableDevices()) {
        devices_.setMidiInputDeviceEnabled(input.identifier, true);
    }
    devices_.addMidiInputDeviceCallback({}, midi_callback_.get());
    return error;
}

void PlaybackEngine::prepare_offline(double sample_rate, int block_size)
{
    prepare_all(sample_rate, block_size);
    rebuild_now();
}

void PlaybackEngine::render_offline(juce::AudioBuffer<float>& out, int num_samples)
{
    out.clear();
    process(out.getArrayOfWritePointers(), out.getNumChannels(), num_samples);
    retire_garbage();
}

void PlaybackEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    // Called before callbacks (re)start, on the thread that opened the
    // device, so instruments can be re-prepared without racing the audio thread.
    prepare_all(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
    rebuild_now();
}

void PlaybackEngine::audioDeviceStopped() {}

void PlaybackEngine::prepare_all(double sr, int bs)
{
    sample_rate_ = sr;
    block_size_ = std::max(bs, 64);
    live_input_.reset(sr);
    // Instruments are re-prepared here; the snapshot that refers to them is
    // replaced immediately after, and the audio thread ignores a snapshot
    // whose block size it cannot honour.
    for (auto& [id, slot] : instruments_)
        if (slot.instrument) slot.instrument->prepare(sample_rate_, block_size_);
}

void PlaybackEngine::request_rebuild() { rebuild_pending_ = true; }

void PlaybackEngine::timerCallback()
{
    if (rebuild_pending_) rebuild_now();
    retire_garbage();
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

std::unique_ptr<PlaybackSnapshot> PlaybackEngine::build_snapshot()
{
    const auto& d = session_.data();
    auto snap = std::make_unique<PlaybackSnapshot>();
    snap->sample_rate = sample_rate_;
    snap->max_block = block_size_;
    snap->master.setSize(2, block_size_);

    std::map<std::string, int> bus_index;
    for (const auto& b : d.buses)
        if (b.kind != "master") bus_index[b.id] = static_cast<int>(bus_index.size());
    snap->buses.resize(bus_index.size());
    for (const auto& b : d.buses) {
        auto it = bus_index.find(b.id);
        if (it == bus_index.end()) continue;
        auto& sb = snap->buses[static_cast<std::size_t>(it->second)];
        sb.gain = db_to_gain(b.gain_db);
        auto out = bus_index.find(b.output);
        sb.output = out == bus_index.end() ? -1 : out->second;
        if (sb.output == it->second) sb.output = -1;
        for (const auto& s : b.sends) {
            auto to = bus_index.find(s.first);
            if (to != bus_index.end() && to->second != it->second) sb.sends.emplace_back(to->second, db_to_gain(s.second));
        }
        for (const auto& ins : b.inserts)
            if (ins.kind == "reverb" && !ins.bypass) {
                sb.reverb = std::make_unique<juce::Reverb>();
                juce::Reverb::Parameters p;
                p.roomSize = 0.82f;
                p.damping = 0.4f;
                p.wetLevel = 1.0f;
                p.dryLevel = 0.0f;
                p.width = 1.0f;
                sb.reverb->setParameters(p);
                sb.reverb->setSampleRate(sample_rate_);
            }
        sb.buffer.setSize(2, block_size_);
    }
    // Deepest buses first, so a bus has received everything before it is mixed on.
    std::vector<std::pair<int, int>> depth;
    for (int i = 0; i < static_cast<int>(snap->buses.size()); ++i) {
        int dpt = 0, cur = i;
        while (snap->buses[static_cast<std::size_t>(cur)].output >= 0 && dpt < 16) {
            cur = snap->buses[static_cast<std::size_t>(cur)].output;
            ++dpt;
        }
        // Effect returns are fed by sends from other buses: process them last.
        if (snap->buses[static_cast<std::size_t>(i)].reverb) dpt = -1;
        depth.emplace_back(-dpt, i);
    }
    std::sort(depth.begin(), depth.end());
    for (const auto& p : depth) snap->bus_order.push_back(p.second);

    std::set<std::string> live_ids;
    for (const project::Track* t : session_.music_tracks()) {
        if (t->kind == project::TrackKind::Reference || t->kind == project::TrackKind::Audio ||
            t->kind == project::TrackKind::Aux)
            continue;
        live_ids.insert(t->id);
        auto& slot = instruments_[t->id];
        PlaybackSnapshot::Track st;
        st.instrument = instrument_for(*t, slot);
        project::RenderedTrack r = project::render_track(*t);
        if (st.instrument->wants_keyswitches()) r.notes.insert(r.notes.end(), r.keyswitches.begin(), r.keyswitches.end());
        st.events = to_events(r.notes, r.controllers, d.tempo, sample_rate_);
        const double pan = juce::jlimit(-1.0, 1.0, t->pan);
        const double angle = (pan + 1.0) * juce::MathConstants<double>::pi / 4.0;
        const float g = db_to_gain(t->gain_db);
        st.gain_left = g * static_cast<float>(std::cos(angle)) * juce::MathConstants<float>::sqrt2;
        st.gain_right = g * static_cast<float>(std::sin(angle)) * juce::MathConstants<float>::sqrt2;
        st.audible = session_.audible(*t);
        auto bi = bus_index.find(t->output_bus_id);
        st.output_bus = bi == bus_index.end() ? -1 : bi->second;
        if (!meter_index_.count(t->id)) meter_index_[t->id] = static_cast<int>(meter_index_.size() % meters_.size());
        st.meter = meter_index_[t->id];
        st.buffer.setSize(2, block_size_);
        st.midi.ensureSize(4096);
        if (t->id == live_track_) snap->live_track = static_cast<int>(snap->tracks.size());
        snap->tracks.push_back(std::move(st));
    }
    // Instruments of deleted tracks: dropped here; snapshots still holding
    // them keep them alive until they are retired on this thread.
    for (auto it = instruments_.begin(); it != instruments_.end();) {
        if (!live_ids.count(it->first)) it = instruments_.erase(it);
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
    if (on_transport_changed) on_transport_changed();
}

void PlaybackEngine::stop()
{
    stop_request_ = true;
    playing_ = false;
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

float PlaybackEngine::take_peak(const std::string& id)
{
    auto it = meter_index_.find(id);
    if (it == meter_index_.end()) return 0.0f;
    return meters_[static_cast<std::size_t>(it->second)].exchange(0.0f);
}

float PlaybackEngine::take_master_peak() { return master_meter_.exchange(0.0f); }

void PlaybackEngine::set_live_track(const std::string& id)
{
    live_track_ = id;
    request_rebuild();
}

// --- Audio thread ---------------------------------------------------------------------------

void PlaybackEngine::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int num_outputs,
                                                      int num_samples, const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < num_outputs; ++ch)
        if (outputs[ch]) juce::FloatVectorOperations::clear(outputs[ch], num_samples);
    process(outputs, num_outputs, num_samples);
}

void PlaybackEngine::process(float* const* outputs, int num_outputs, int num_samples) noexcept
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

    const int max_block = current_ ? current_->max_block : num_samples;
    for (int offset = 0; offset < num_samples; offset += max_block)
        process_chunk(outputs, num_outputs, offset, std::min(max_block, num_samples - offset));
}

void PlaybackEngine::process_chunk(float* const* outputs, int num_outputs, int offset, int n) noexcept
{
    PlaybackSnapshot* s = current_;
    const bool stopping = stop_request_.exchange(false);
    const std::int64_t locate = locate_request_.exchange(-1);
    if (play_request_.exchange(false) && s) s->seek(position_samples_.load());
    if (s && (stopping || locate >= 0)) {
        for (auto& t : s->tracks) t.instrument->all_notes_off();
        if (locate >= 0) {
            position_samples_ = locate;
            s->seek(locate);
        }
    }
    const bool playing = playing_.load();
    const std::int64_t pos = position_samples_.load();

    if (s && n <= s->max_block) {
        s->master.clear(0, n);
        for (auto& b : s->buses) b.buffer.clear(0, n);
        for (std::size_t i = 0; i < s->tracks.size(); ++i) {
            auto& t = s->tracks[i];
            t.midi.clear();
            if (playing) gather(t.events, t.cursor, pos, n, t.midi);
            if (static_cast<int>(i) == s->live_track) live_input_.removeNextBlockOfMessages(t.midi, n);
            juce::AudioBuffer<float> buf(t.buffer.getArrayOfWritePointers(), 2, n);
            buf.clear();
            t.instrument->process(buf, t.midi, n);
            if (!t.audible) continue;
            buf.applyGain(0, 0, n, t.gain_left);
            buf.applyGain(1, 0, n, t.gain_right);
            add_peak(meters_[static_cast<std::size_t>(t.meter)], buf, n);
            juce::AudioBuffer<float>& dest = t.output_bus >= 0 ? s->buses[static_cast<std::size_t>(t.output_bus)].buffer : s->master;
            for (int ch = 0; ch < 2; ++ch) dest.addFrom(ch, 0, buf, ch, 0, n);
        }
        for (int bi : s->bus_order) {
            auto& b = s->buses[static_cast<std::size_t>(bi)];
            if (b.reverb) b.reverb->processStereo(b.buffer.getWritePointer(0), b.buffer.getWritePointer(1), n);
            b.buffer.applyGain(0, n, b.gain);
            for (const auto& send : b.sends)
                for (int ch = 0; ch < 2; ++ch)
                    s->buses[static_cast<std::size_t>(send.first)].buffer.addFrom(ch, 0, b.buffer, ch, 0, n, send.second);
            juce::AudioBuffer<float>& dest = b.output >= 0 ? s->buses[static_cast<std::size_t>(b.output)].buffer : s->master;
            for (int ch = 0; ch < 2; ++ch) dest.addFrom(ch, 0, b.buffer, ch, 0, n);
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
                    s->master.addSample(0, at + k, v);
                    s->master.addSample(1, at + k, v);
                }
            }
        } else if (s) {
            // Keep the click cursor in step while it is off.
            while (s->click_cursor < s->clicks.size() && s->clicks[s->click_cursor].first < pos + n) ++s->click_cursor;
        }
        add_peak(master_meter_, s->master, n);
        for (int ch = 0; ch < num_outputs && ch < 2; ++ch)
            if (outputs[ch]) juce::FloatVectorOperations::add(outputs[ch] + offset, s->master.getReadPointer(ch), n);
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
