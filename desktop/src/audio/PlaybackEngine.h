// Playback: the project's instrument tracks through their instruments, mix
// and buses to the audio device (Main Spec §12).
//
// Real-time discipline (carried over from the original VC Composer): the
// audio callback never allocates, locks, logs or touches the document.
//   * On the message thread, every document change is rendered into an
//     immutable PlaybackSnapshot - MIDI events at sample positions, gains,
//     routing, click positions and pre-sized buffers.
//   * The snapshot is handed over through an atomic pointer. The audio thread
//     adopts it at the start of a block and hands the old one back through a
//     lock-free queue; the message thread deletes it. Instruments are shared
//     by snapshots and so are only ever destroyed on the message thread.
//   * Transport requests (play, stop, locate) are atomics read once a block.
//
// The same processing runs without a device (render_offline) so tests and
// the headless check can verify that a project actually makes sound.
//
// Console (Main Spec §12): every track and bus (master included) runs its
// channel strip (ChannelDsp.h), inserts, fader and pan, sends and metering.
// Processor objects persist across snapshot rebuilds in message-thread maps
// keyed by strip id (and insert kind/occurrence), like the instruments, so a
// document edit never cuts a reverb tail or resets a compressor; settings
// reach them lock-free (Mailbox) and gains are ramped per block, so a knob
// drag is immediate and click-free. Buses run in dependency order; a route
// or send that would make a loop is ignored. Solo is solo-in-place: a
// soloed strip keeps the buses it feeds (and the strips feeding a soloed bus)
// audible, fx returns are solo-safe.
//
// Audio clips (Main Spec §11) stream from disk (DiskStreamer.h), converted to
// the device rate as they are read. Recording writes each armed track's input to a 24-bit WAV
// through ThreadedWriter (lock-free from the audio thread); the pass is
// placed as one "Record" undo step on the message thread when it ends.
//
// Automation (Main Spec §12): every enabled lane of a track or bus whose mode
// is not "off" is rendered into the snapshot in samples (the tempo map is
// applied on the message thread) and evaluated on the audio thread each
// block: fader, pan and send gains ramp across the block to the lane's value
// at its end; strip, insert and plug-in parameters change per block (a
// plug-in parameter only when its value changes); a mute lane is applied
// sample-accurately, closing over 1 ms that ends exactly on its point. An
// automated value overrides the static one. Writing (touch / latch / write)
// is captured on the message thread against the heard playhead; the value
// being written reaches the sound through a lock-free slot; the pass is
// committed as one "Write automation" undo step on stop or locate. Gestures
// in a plug-in's own editor are captured too (ParameterWatch).
//
// Plug-in delay compensation: a strip's latency is its instrument's plus its
// inserts' (a bypassed plug-in keeps its latency). Each routing edge (track
// to bus, send, bus to bus) is delayed so every path reaches the master at
// the same time; the click is scheduled late by the same amount. The
// playhead (position(), position_seconds()) reports what is heard: the
// render position minus the compensation, so picture and meters follow the
// sound. Recording places takes against the heard position. Low-latency
// monitoring: a track monitoring its audio input, and the live-MIDI track
// while stopped or record-armed, skip their compensation delay (what the
// performer plays is heard at once; such a track's own playback is then
// early by the compensation while it monitors).
#pragma once

#include <array>

#include "audio/SurfaceInput.h"
#include <atomic>
#include <deque>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "audio/AutomationRt.h"
#include "audio/Instrument.h"
#include "audio/MidiOut.h"
#include "audio/PluginHost.h"
#include "vcc/project/RenderCache.h"
#include "vcc/audio/Loudness.h"
#include "vcc/audio/SpscRingBuffer.h"
#include "vcc/core/Midi.h"
#include "vcc/session/Session.h"

namespace vcc::desktop {

struct PlaybackSnapshot;
struct PreviewClip;
struct StripDsp;
struct RecordPass;
struct AutoSlot;  // per automated (owner, param): the value being written, lock-free
class InsertProcessor;
class DiskStreamer;
class ClipStream;

class PlaybackEngine final : public juce::AudioIODeviceCallback, private juce::Timer {
public:
    PlaybackEngine(session::Session& session, PluginHost& plugins);
    ~PlaybackEngine() override;

    /// Opens the default output device. Returns an empty string, or why there
    /// is no audio (the app keeps working silently).
    /// With `saved` (a state from device_state()), the interface, sample
    /// rate, buffer, channels and MIDI inputs chosen last time, falling back
    /// to the defaults when that interface is not connected.
    juce::String open_default_device(const juce::XmlElement* saved = nullptr);
    juce::AudioDeviceManager& devices() noexcept { return devices_; }
    /// What open_default_device takes back: the device setup and MIDI inputs.
    std::unique_ptr<juce::XmlElement> device_state() const;
    /// Called (message thread) whenever the device setup or the MIDI inputs change.
    std::function<void()> on_devices_changed;
    /// MIDI inputs plugged in while the app runs (USB, USB-C, Bluetooth, a
    /// network session) are turned on as they arrive; off: they wait to be
    /// turned on in MIDI Setup. The ones seen before keep their choice.
    void set_auto_enable_midi(bool on) noexcept { auto_enable_midi_ = on; }
    bool auto_enable_midi() const noexcept { return auto_enable_midi_; }
    /// The MIDI inputs seen so far (identifiers), saved with the setup.
    const juce::StringArray& known_midi_inputs() const noexcept { return known_midi_; }
    /// Looks at the connected MIDI inputs now (also done on every plug and unplug).
    void refresh_midi_inputs();

    /// Prepares for offline rendering without a device: an open device stops
    /// driving this engine (the caller renders every block).
    void prepare_offline(double sample_rate, int block_size);
    /// Renders the next `num_samples` exactly as the device callback would.
    void render_offline(juce::AudioBuffer<float>& out, int num_samples);

    // --- Transport ---------------------------------------------------------------
    void play();
    void stop();
    void toggle_play();
    bool playing() const noexcept { return playing_.load(); }
    void locate(Ticks tick);
    /// Playhead in project ticks (through the project's tempo map).
    Ticks position() const;
    double position_seconds() const;
    /// Audio scrub (Add-On spec §7): tape-style varispeed. While playing at a
    /// rate other than 1 the whole mix is rendered rate times faster (or
    /// slower) and resampled, so pitch follows speed, as a tape transport
    /// does. 0.25..4; 1 is normal playback. Not while recording.
    void set_varispeed(double rate) noexcept;
    double varispeed() const noexcept { return varispeed_.load(); }
    /// A short audible grain for the jog and reverse shuttle: plays from
    /// `at` for `seconds`, then stops and returns the playhead to `at`.
    void scrub_grain(Ticks at, double seconds);
    bool grain_playing() const noexcept { return grain_active_; }
    void set_metronome(bool on) noexcept { metronome_ = on; }
    bool metronome() const noexcept { return metronome_.load(); }

    // --- Audition (Chord Explorer, Ostinato Creator, template preview) -----------
    /// Plays notes through a placeholder voice, independent of the transport
    /// and the project: nothing is written (Supplement §9.1, §10.2, T05).
    void preview(const std::vector<NoteEvent>& notes, const std::string& family, int gm_program, double bpm,
                 bool loop, Ticks loop_length = 0);
    /// The same through a plug-in (the library browser's audition, Main Spec
    /// §10.5): a temporary instance with the reference's state, outside the
    /// project's tracks. False, with why, when it cannot be loaded.
    bool preview_plugin(const project::PluginRef& plugin, const std::vector<NoteEvent>& notes, double bpm,
                        juce::String& error);
    void stop_preview();
    bool previewing() const noexcept { return previewing_.load(); }

    // --- Instruments and meters ---------------------------------------------------
    /// What is playing a track: "Placeholder (bowed)", a plug-in name, or why
    /// a saved plug-in could not be loaded.
    juce::String instrument_status(const std::string& track_id) const;
    juce::AudioPluginInstance* plugin_for(const std::string& track_id) const;
    /// Per instrument, for the usage panel (Memory Management spec §6.1):
    /// its share of the engine's real-time budget since the last call, and
    /// where it runs. Message thread.
    struct InstrumentUsage {
        std::string track_id;
        juce::String name;             ///< What plays: a plug-in, a placeholder, "Unloaded...".
        bool plugin = false;           ///< A hosted plug-in (not the placeholder).
        bool loaded = true;            ///< False: unloaded (frozen) or played by articulation tracks.
        std::uintptr_t host_process = 0;  ///< Its plug-in host process; 0: inside the app.
        double load_percent = 0.0;     ///< Its processing time / real time, since the last call.
        bool measured = false;         ///< False until it has processed audio in a measured period.
    };
    std::vector<InstrumentUsage> instrument_usage();
    /// Reload: the instrument is closed and opened again from its saved
    /// settings (captured first by the caller). False when it has none.
    bool reload_instrument(const std::string& track_id);
    /// Peak level since the last call, 0..1+, for the mixer meters.
    float take_peak(const std::string& track_id);
    float take_master_peak();

    // --- Console metering (Main Spec §12) -----------------------------------------
    struct StripMeter {
        float left = 0.0f, right = 0.0f;  ///< Peak since the last call, linear (1.0 = 0 dBFS).
        float gain_reduction_db = 0.0f;   ///< Channel compressor, 0 or negative.
        bool gate_closed = false;
        bool clipped = false;             ///< Went over 0 dBFS since the last call.
    };
    /// For a track id, a bus id or "bus-master". Unknown ids read silent.
    StripMeter take_meter(const std::string& strip_id);
    /// The same peaks (left and right only) for the Edit window's own
    /// displays, read independently of `take_meter` and of each other:
    /// reader 0 the top deck and track headers, 1 the channel strip, 2 the
    /// mixer deck.
    static constexpr int kMeterReaders = 6;  // composer, inspector, mixer deck, routing canvas, mixer controller, rack VUs
    /// The master output's loudness, 100 ms at a time while the transport
    /// plays (BS.1770 K-weighted mean square and true peak), for the Master
    /// panel's LUFS and dBTP readings. Drained by the message thread.
    std::vector<audio::LoudnessBlock> take_loudness_blocks();
    // --- External MIDI and real-time print -------------------------------------------
    /// External MIDI out: a MIDI track with a `midi_output` sends its notes
    /// there (through the router) instead of to its instrument, and hears the
    /// hardware on its audio input.
    MidiOutRouter& midi_router() noexcept { return *midi_router_; }
    /// A control surface's MIDI: offered every incoming message first.
    SurfaceInput& surface_input() noexcept { return surface_input_; }
    /// The mixer controller's input (Mackie Control): its whole device.
    SurfaceInput& mixer_surface_input() noexcept { return mixer_surface_input_; }

    // --- MIDI input filter and retrospective record (Main Spec §9) --------------
    /// What MIDI input is let through to play and record.
    struct InputFilter {
        std::uint16_t channels = 0xFFFF;  ///< Bit n: channel n+1.
        std::uint8_t note_low = 0, note_high = 127, velocity_min = 1;
        bool notes = true, controllers = true, pitch_bend = true, aftertouch = true, program = true;
    };
    void set_input_filter(const InputFilter& f) noexcept { input_filter_.store(pack(f)); }
    InputFilter input_filter() const noexcept { return unpack(input_filter_.load()); }
    static std::uint64_t pack(const InputFilter& f) noexcept
    {
        return std::uint64_t(f.channels) | (std::uint64_t(f.note_low & 127) << 16) | (std::uint64_t(f.note_high & 127) << 23) |
               (std::uint64_t(f.velocity_min & 127) << 30) | (std::uint64_t(f.notes) << 37) |
               (std::uint64_t(f.controllers) << 38) | (std::uint64_t(f.pitch_bend) << 39) |
               (std::uint64_t(f.aftertouch) << 40) | (std::uint64_t(f.program) << 41);
    }
    static InputFilter unpack(std::uint64_t v) noexcept
    {
        InputFilter f;
        f.channels = static_cast<std::uint16_t>(v & 0xFFFF);
        f.note_low = static_cast<std::uint8_t>((v >> 16) & 127);
        f.note_high = static_cast<std::uint8_t>((v >> 23) & 127);
        f.velocity_min = static_cast<std::uint8_t>((v >> 30) & 127);
        f.notes = (v >> 37) & 1;
        f.controllers = (v >> 38) & 1;
        f.pitch_bend = (v >> 39) & 1;
        f.aftertouch = (v >> 40) & 1;
        f.program = (v >> 41) & 1;
        return f;
    }
    static bool passes(const InputFilter& f, const juce::MidiMessage& m) noexcept;
    /// MIDI thread: false when the filter drops the message. Logs it for
    /// the retrospective record.
    bool accept_live(const juce::MidiMessage& m) noexcept;
    /// Whether a message from `device` may play the selected track (its MIDI In). Any thread.
    bool live_input_allows(const juce::String& device) const;
    /// What was played on MIDI inputs while not recording (the last ten
    /// minutes): wall time, and the transport position when it was playing.
    struct LiveEvent {
        std::uint8_t bytes[3] = {0, 0, 0};
        double wall_ms = 0.0;
        std::int64_t position = -1;  ///< Samples, or -1 when stopped (or recording).
    };
    std::vector<LiveEvent> live_history();
    /// The rate the engine runs at (live history positions are in its samples).
    double sample_rate() const noexcept { return sample_rate_; }
    void clear_live_history();

    // --- Monitor path (Main Spec §12) -------------------------------------------
    /// What the speakers hear, never what renders and exports write: dim,
    /// mono fold, mute, and which output pair (0 = outputs 1-2) carries the
    /// mix. Cue buses keep their own outputs.
    struct Monitor {
        bool dim = false;
        double dim_db = -20.0;
        bool mono = false;
        bool mute = false;
        int pair = 0;
    };
    void set_monitor(const Monitor& m) noexcept
    {
        monitor_dim_.store(m.dim);
        monitor_dim_gain_.store(static_cast<float>(std::pow(10.0, m.dim_db / 20.0)));
        monitor_dim_db_ = m.dim_db;
        monitor_mono_.store(m.mono);
        monitor_mute_.store(m.mute);
        monitor_pair_.store(m.pair);
    }
    Monitor monitor() const noexcept
    {
        return {monitor_dim_.load(), monitor_dim_db_, monitor_mono_.load(), monitor_mute_.load(), monitor_pair_.load()};
    }

    // --- Learn (Articulation Map window) -----------------------------------------
    /// The next note-on, controller or program change from any MIDI input is
    /// captured (not played) and handed to take_learned().
    void arm_learn() noexcept { learned_.store(0, std::memory_order_relaxed); learn_armed_.store(true); }
    void cancel_learn() noexcept { learn_armed_.store(false); }
    bool learning() const noexcept { return learn_armed_.load(); }
    /// MIDI thread (or a check): true when the message was taken.
    bool offer_learn(const juce::MidiMessage& m) noexcept
    {
        if (!learn_armed_.load(std::memory_order_acquire)) return false;
        if (!(m.isNoteOn() || m.isController() || m.isProgramChange())) return m.isNoteOff();
        const auto* d = m.getRawData();
        const std::uint32_t packed = 0x1000000u | (std::uint32_t(d[0]) << 16) | (std::uint32_t(d[1]) << 8) |
                                     (m.getRawDataSize() > 2 ? d[2] : 0u);
        learned_.store(packed, std::memory_order_release);
        learn_armed_.store(false, std::memory_order_release);
        return true;
    }
    /// Message thread: the captured message's three bytes, once.
    std::optional<std::array<std::uint8_t, 3>> take_learned() noexcept
    {
        const std::uint32_t v = learned_.exchange(0, std::memory_order_acq_rel);
        if (!(v & 0x1000000u)) return std::nullopt;
        return std::array<std::uint8_t, 3>{std::uint8_t((v >> 16) & 0xFF), std::uint8_t((v >> 8) & 0xFF),
                                           std::uint8_t(v & 0xFF)};
    }
    /// Turns on a MIDI input plugged in after the device was opened.
    void enable_midi_input(const juce::String& identifier)
    {
        if (!devices_.isMidiInputDeviceEnabled(identifier)) devices_.setMidiInputDeviceEnabled(identifier, true);
    }
    /// Print to audio in real time: the transport plays the range live and
    /// each source is recorded as it plays - an instrument track's
    /// instrument (before the strip, or after its inserts), or a MIDI
    /// track's audio input (its hardware's return, the round-trip latency
    /// taken off). Returns an empty string, or why it cannot start.
    struct LivePrintSource {
        std::string track_id;
        juce::File file;
        bool from_input = false;
        bool post_inserts = false;
    };
    struct LivePrintResult {
        std::string track_id;
        juce::File file;
        bool ok = false;
        juce::String message;
        double sample_rate = 48000.0;
        double source_offset = 0.0;  ///< Seconds of latency at the file's start, skipped by the clip.
        double length = 0.0;         ///< Seconds, the range and its ringing tail.
        float peak = 0.0f;
    };
    juce::String start_live_print(const std::vector<LivePrintSource>& sources, Ticks start, Ticks end, double tail_seconds);
    bool live_printing() const noexcept { return live_ != nullptr; }
    double live_print_progress() const noexcept;
    /// Stops a real-time print and throws its files away.
    void cancel_live_print();
    /// Message thread: finishes a print whose range has played (or that was
    /// stopped); the timer calls it.
    void service_live_print();
    std::function<void(const std::vector<LivePrintResult>& results, bool cancelled)> on_live_print_finished;

    /// Starts the analysis afresh (with the meter's reset).
    void reset_loudness() noexcept { loudness_reset_ = true; }
    StripMeter take_deck_meter(const std::string& strip_id, int reader = 0);
    /// Device inputs (for input selectors and meters).
    int num_inputs() const;
    juce::String input_name(int channel) const;
    float take_input_peak(int channel);

    // --- Recording (Main Spec §11) -------------------------------------------------
    /// Transport record arm. Recording runs while playing with this on, onto
    /// every record-armed track (inside the punch range when punch is on).
    void set_record_enabled(bool on);
    bool record_enabled() const noexcept;
    bool recording() const noexcept;
    void set_punch(bool on, Ticks in, Ticks out);
    /// Before a recording starts: `bars` of count-in (the click alone, the
    /// music silent) or, with `pre_roll`, of the music playing; recording
    /// begins where the transport was. 0 bars: straight in.
    void set_count_in(int bars, bool pre_roll) noexcept;
    int count_in_bars() const noexcept { return count_in_bars_.load(); }
    bool pre_roll() const noexcept { return pre_roll_.load(); }
    /// Wait for note: with record on, Play waits until the first note on the
    /// MIDI keyboard (or the deck) and starts there.
    void set_wait_note(bool on) noexcept { wait_note_ = on; }
    bool wait_note() const noexcept { return wait_note_.load(); }
    bool waiting_for_note() const noexcept { return waiting_for_note_.load(); }
    /// Message thread: starts a pass a waited-for note triggered (the timer calls it).
    void poll_wait_note();
    /// Message thread: collects finished takes, starts a waited-for pass and
    /// opens the next loop cycle's take ahead of the wrap (the timer calls it;
    /// the checks call it between offline blocks).
    void service_recording();
    /// Heard sample where the current pass records from (count-in/pre-roll end).
    std::int64_t record_from_sample() const noexcept { return record_from_; }
    bool punch() const noexcept;
    /// Loop playback: on reaching `out` while playing, the playhead jumps
    /// back to `in`, sample-accurately (notes off, delay lines cleared, as
    /// any jump). Starting after `out` plays on.
    void set_loop(bool on, Ticks in, Ticks out);
    bool loop() const noexcept { return loop_.load(); }
    std::pair<Ticks, Ticks> loop_range() const noexcept { return {loop_in_ticks_, loop_out_ticks_}; }
    /// Seconds recorded in the current pass.
    double record_seconds() const noexcept;
    /// Called on the message thread after a pass is placed (one undo step),
    /// or with an error ("disk full", "no input device").
    std::function<void(const juce::String& message, bool ok)> on_recording_finished;
    /// Offline rendering with a given input (tests and the headless check):
    /// `input` stands in for the device's input channels.
    void render_offline_with_input(const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& out,
                                   int num_samples);

    /// Why a track's audio clips are not all playing ("Missing audio file: x.wav"), or empty.
    juce::String audio_status(const std::string& track_id) const;
    /// Why an insert slot is not processing (missing plug-in), or empty.
    juce::String insert_status(const std::string& strip_id, int slot) const;
    /// The hosted effect in an insert slot (to open its editor or store its state).
    juce::AudioPluginInstance* insert_plugin(const std::string& strip_id, int slot) const;
    /// Keeps a track's instrument (`slot` < 0) or an insert's plug-in alive
    /// while held, so a plug-in window outlives a rebuild that drops it and
    /// closes first (its editor talks to the plug-in as it goes).
    std::shared_ptr<void> plugin_keepalive(const std::string& owner_id, int slot) const;

    // --- Sound Setup slot -------------------------------------------------------------
    /// A channel outside the project for setting up sounds: it plays this
    /// plug-in (with its state, if any) to the master, and the MIDI keyboard
    /// plays it while it is the live track. An empty reference removes it.
    static constexpr const char* kSetupTrackId = "__sound_setup__";
    /// The plug-in host processes in use, with a plug-in each carries and
    /// what it is on (message thread): for telling a frozen one.
    struct RemoteHost {
        std::shared_ptr<class HostProcess> process;
        juce::String plugin_name;
        std::string owner_id;
    };
    std::vector<RemoteHost> remote_hosts() const;
    void set_setup_instrument(const project::PluginRef& plugin);
    bool has_setup_instrument() const noexcept { return setup_track_ != nullptr; }
    /// The processor in an insert slot, or null (message thread; look it up
    /// each time - a rebuild replaces it). The rack reads its live meters.
    InsertProcessor* insert_processor(const std::string& strip_id, int slot) const;
    /// Deliverable rendering: only the tracks a deliverable includes play
    /// (reference/temp tracks are left out unless opted in).
    void set_export_mode(bool on);
    /// Blocks where a clip had to play silence because the disk had not
    /// delivered its audio yet, since the last call (0 in a healthy session).
    int take_disk_underruns() noexcept;
    // --- Plug-ins in their own processes ------------------------------------------------
    /// Plug-ins whose process has died since the last call (names, each
    /// reported once), and blocks played as silence because a plug-in was
    /// late. The main window turns these into a status message.
    juce::StringArray take_newly_crashed_plugins();
    int take_late_plugin_blocks();
    /// Starts every crashed plug-in again from its last state. Returns how many.
    int reload_crashed_plugins();
    /// Stores every hosted plug-in's state in the project (instruments and
    /// effect inserts, on tracks and buses), without an undo step.
    void capture_plugin_states();
    /// Stops and reloads every plug-in from its stored state - after the
    /// hosting mode changes. Returns how many were restarted.
    int restart_plugins();

    /// Offline rendering streams synchronously (prepare_offline turns it
    /// on); the checks turn it off to exercise the real-time disk thread.
    void set_disk_synchronous(bool on);
    /// Stem printing: while set, these strips (track or bus ids) are the
    /// only ones soloed - the document's own solo switches are ignored, mute
    /// switches still count - and with `mute_effects` every fx return is
    /// muted (a dry stem). Render-only: never touches the document.
    struct RenderOverride {
        std::vector<std::string> solo;
        bool mute_effects = false;
    };
    void set_render_override(std::optional<RenderOverride> override_settings);
    const std::optional<RenderOverride>& render_override() const noexcept { return render_override_; }
    /// Print to audio (render-only, offline): only this track is rendered,
    /// from its MIDI alone (its clips are left out), and the output is its
    /// instrument's signal - before the channel strip, or after the strip's
    /// processing and inserts with `post_inserts` - never its fader, pan,
    /// sends or the buses.
    struct PrintTap {
        std::string track_id;
        bool post_inserts = false;
    };
    void set_print_tap(std::optional<PrintTap> tap);
    /// How late the tapped signal comes out: the instrument's latency, plus
    /// the inserts' with `post_inserts`.
    int print_tap_latency() const noexcept { return print_tap_latency_; }

    // --- Automation (Main Spec §12) ------------------------------------------------
    // `owner_id` is a track id or bus id ("bus-master"); `param` as in
    // project::AutomationLane (vcc/project/Automation.h).
    /// What the parameter is playing at now when a lane drives it (mode read,
    /// touch, latch; or the value being written), so controls can follow it.
    /// Empty when the parameter is not automated or the mode is "off".
    std::optional<double> automated_value(const std::string& owner_id, const std::string& param) const;
    /// Console gestures. While the transport plays and the owner's mode is
    /// touch, latch or write, the engine records the values it is given; on
    /// stop the pass is committed through Session::commit_automation_pass
    /// (one undo step) and on_automation_written is called. The engine also
    /// uses the touched value for sound while writing.
    void begin_touch(const std::string& owner_id, const std::string& param);
    void touch_value(const std::string& owner_id, const std::string& param, double value);
    void end_touch(const std::string& owner_id, const std::string& param);
    /// True when a gesture on this control would be written now.
    bool writes_automation(const std::string& owner_id, const std::string& param) const;
    std::function<void(const juce::String& message)> on_automation_written;

    // --- Plug-in delay compensation ---------------------------------------------------
    void set_delay_compensation(bool on);
    bool delay_compensation() const noexcept;
    /// Processing latency of a strip (its instrument and inserts), samples.
    int latency_samples(const std::string& strip_id) const;
    /// The delay the engine adds to line everything up (the longest path).
    /// 0 while compensation is off.
    int compensation_samples() const;

    /// Re-render the snapshot now (normally debounced after document changes).
    void rebuild_now();

    /// Live MIDI input is played on this track's instrument.
    void set_live_track(const std::string& track_id);
    /// Plays a message on the live track as if from a MIDI keyboard (the
    /// on-screen keys and wheels). Not shifted by the live octave.
    void send_live(const juce::MidiMessage& m);
    /// Octaves added to notes from MIDI input devices (the deck's Octave keys).
    void set_live_octave(int octaves) noexcept { live_octave_ = octaves; }
    int live_octave() const noexcept { return live_octave_.load(); }
    /// A live note (device or on-screen) is held down.
    bool live_held(int note) const noexcept
    {
        return note >= 0 && note < 128 && live_held_[static_cast<std::size_t>(note)].load(std::memory_order_relaxed) > 0;
    }
    /// The message thread's view of a live message passing through (both sources).
    void note_live(const juce::MidiMessage& m) noexcept;

    std::function<void()> on_transport_changed;

    // juce::AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext(const float* const* inputs, int num_inputs, float* const* outputs,
                                          int num_outputs, int num_samples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    /// A parameter being written in the current pass (message thread).
    struct WriteSegment {
        std::string key, owner, param, name;
        Ticks from = 0;
        std::vector<project::AutomationPoint> points;
        std::shared_ptr<AutoSlot> slot;
        double value = 0.0;
        bool held = false;         ///< The control is being touched.
        bool from_static = false;  ///< Write mode, untouched: writes the static value.
    };
    struct InstrumentSlot {
        std::string key;  ///< what it was built from; rebuilt when this changes
        std::shared_ptr<Instrument> instrument;
        juce::String status;
        std::int64_t seen_ticks = 0, seen_samples = 0;  ///< At the last usage reading.
    };

    void timerCallback() override;
    void request_rebuild();
    void prepare_all(double sample_rate, int block_size);
    std::shared_ptr<Instrument> instrument_for(const project::Track& track, InstrumentSlot& slot);
    std::unique_ptr<PlaybackSnapshot> build_snapshot();
    void process(const float* const* inputs, int num_inputs, float* const* outputs, int num_outputs,
                 int num_samples) noexcept;
    void process_chunk(const float* const* inputs, int num_inputs, float* const* outputs, int num_outputs, int offset,
                       int num_samples) noexcept;
    void retire_garbage();
    /// Creates/updates the persistent strip and insert processors and pushes
    /// every mix setting to them (lock-free). Cheap; runs on every edit.
    void sync_processors();
    // Recording
    void begin_pass();
    void end_passes();
    void service_passes() noexcept;  // audio side: adopt / stop passes
    void retire_active_pass() noexcept;
    void collect_finished_passes();
    void finalize_pass(std::unique_ptr<RecordPass> pass);
    void report_recording(const juce::String& message, bool ok);
    // Automation (message thread)
    std::shared_ptr<AutoSlot> slot_for(const std::string& owner, const std::string& param, bool* created = nullptr);
    std::string mode_of(const std::string& owner) const;
    std::optional<double> static_of(const std::string& owner, const std::string& param) const;
    std::string param_name(const std::string& owner, const std::string& param) const;
    std::string owner_name(const std::string& owner) const;
    Ticks tick_at_sample(std::int64_t sample) const;
    WriteSegment* find_segment(const std::string& key);
    static void add_point(WriteSegment& seg, Ticks tick, double value);
    bool open_segment(const std::string& owner, const std::string& param, double value, bool held, bool from_static,
                      std::int64_t at);
    void close_segment(std::size_t index, std::int64_t at);
    void begin_touch_at(const std::string& owner, const std::string& param, std::int64_t at);
    void touch_value_at(const std::string& owner, const std::string& param, double value, std::int64_t at);
    void end_touch_at(const std::string& owner, const std::string& param, std::int64_t at);
    void start_automation_pass();
    void finish_automation_pass();
    void refresh_writes();
    void drain_gestures();
    void watch_plugin(juce::AudioPluginInstance& instance, const void* holder, bool has_watch,
                      const std::function<void(std::unique_ptr<ParameterWatch>)>& attach,
                      std::map<int, std::pair<std::string, std::string>>& targets, const std::string& owner,
                      const std::string& prefix);
    std::vector<int> current_latencies() const;

    session::Session& session_;
    PluginHost& plugins_;
    juce::AudioDeviceManager devices_;
    int listener_ = 0;
    int sounds_listener_ = 0, sounds_generation_ = 0;

    double sample_rate_ = 48000.0;
    int block_size_ = 512;

    std::unique_ptr<project::Track> setup_track_;  ///< The Sound Setup slot, outside the project.
    std::vector<const project::Track*> tracks_to_play() const;
    std::map<std::string, InstrumentSlot> instruments_;  // message thread only
    std::map<std::string, std::shared_ptr<StripDsp>> strips_;                 // strip id -> processing, meters
    std::map<std::string, std::shared_ptr<InsertProcessor>> inserts_;         // strip/kind/plugin#n -> processor
    std::map<std::string, std::vector<std::string>> insert_keys_;             // strip id -> key per slot
    std::unique_ptr<DiskStreamer> files_;
    std::set<const void*> crash_reported_;  // message thread
    std::map<std::string, std::shared_ptr<ClipStream>> streams_;  // clip key -> stream, message thread
    std::map<std::string, juce::String> audio_status_;
    bool export_mode_ = false;
    std::optional<RenderOverride> render_override_;
    std::optional<PrintTap> print_tap_;
    struct LivePrint;
    std::unique_ptr<LivePrint> live_;                  // message thread
    std::atomic<LivePrint*> live_ptr_{nullptr};        // handed to the audio thread
    LivePrint* live_rt_ = nullptr;                     // audio thread, per callback
    std::atomic<bool> live_release_{false}, live_released_{false};
    bool live_cancel_ = false;
    std::vector<std::string> live_takes_ids_;  ///< The print's tracks, by slot (message thread).
    std::unique_ptr<MidiOutRouter> midi_router_;
    bool offline_ = true;
    bool prepared_offline_ = false;  ///< Driven by render_offline (the checks, bounces), not a device.
    std::atomic<int> output_latency_{0};
    std::map<std::string, int> instrument_latency_;
    audio::LoudnessAnalyser loudness_;                    // audio thread, once prepared
    audio::SpscQueue<audio::LoudnessBlock> loudness_blocks_{512};
    std::atomic<bool> loudness_reset_{false};
    // Count-in, pre-roll, wait note, MIDI recording, loop takes.
    std::atomic<int> count_in_bars_{0};
    std::atomic<bool> pre_roll_{false}, wait_note_{false}, waiting_for_note_{false}, wait_triggered_{false};
    std::uint8_t wait_note_bytes_[3] = {0, 0, 0};
    std::int64_t record_from_ = std::numeric_limits<std::int64_t>::min();  // message thread, for the next pass
    std::atomic<std::int64_t> count_in_until_{std::numeric_limits<std::int64_t>::min()};
    std::array<std::pair<std::int64_t, bool>, 64> count_in_clicks_{};
    std::int64_t roll_origin_ = std::numeric_limits<std::int64_t>::min();  // where a count-in returns to on stop
    std::uint8_t pending_first_note_[3] = {0, 0, 0};
    void start_play(bool from_wait);
    void discard_pass(RecordPass* pass);
    std::atomic<int> count_in_click_count_{0};
    struct CapturedMidi {
        std::uint64_t pass = 0;
        std::int64_t sample = 0;  ///< Heard timeline.
        std::uint8_t bytes[3] = {0, 0, 0};
        int size = 0;
    };
    audio::SpscQueue<CapturedMidi> midi_capture_{8192};
    std::vector<CapturedMidi> midi_backlog_;  // message thread: drained, for passes not finalized yet
    juce::MidiBuffer live_block_;             // audio thread
    std::atomic<RecordPass*> standby_pass_{nullptr};  // the next loop cycle's take, opened ahead
    std::unique_ptr<RecordPass> make_pass(juce::String& error);
    int place_midi(const RecordPass& pass);  // notes placed
    int print_tap_latency_ = 0;
    std::array<std::atomic<float>, 64> input_meters_{};
    std::atomic<int> num_inputs_{0};

    // Handover
    std::atomic<PlaybackSnapshot*> pending_{nullptr};
    PlaybackSnapshot* current_ = nullptr;  // audio thread
    PlaybackSnapshot* spill_ = nullptr;    // audio thread: retired but the queue was full
    audio::SpscQueue<PlaybackSnapshot*> retired_{64};
    std::atomic<PreviewClip*> pending_preview_{nullptr};
    PreviewClip* preview_ = nullptr;  // audio thread
    audio::SpscQueue<PreviewClip*> retired_previews_{16};
    std::atomic<bool> stop_preview_request_{false};

    // Transport
    std::atomic<bool> playing_{false};
    std::atomic<bool> play_request_{false};
    std::atomic<bool> stop_request_{false};
    std::atomic<bool> loop_{false};
    std::atomic<std::int64_t> loop_in_{0}, loop_out_{0};  ///< Samples.
    Ticks loop_in_ticks_ = 0, loop_out_ticks_ = 0;
    std::atomic<std::int64_t> locate_request_{-1};
    std::atomic<std::int64_t> position_samples_{0};
    std::atomic<bool> metronome_{false};
    std::atomic<bool> previewing_{false};
    bool rebuild_pending_ = false;

    // Recording: a pass is created on the message thread, adopted by the
    // audio thread, and handed back when it ends (stop, punch out, locate).
    std::atomic<bool> record_enabled_{false};
    std::atomic<bool> punch_{false};
    Ticks punch_in_ = 0, punch_out_ = 0;
    std::uint64_t next_pass_id_ = 1;
    std::atomic<std::uint64_t> stop_passes_upto_{0};
    std::atomic<RecordPass*> pending_pass_{nullptr};
    RecordPass* active_pass_ = nullptr;  // audio thread
    std::atomic<RecordPass*> finished_pass_{nullptr};
    std::atomic<bool> pass_active_{false};
    std::atomic<bool> recording_{false};
    std::atomic<std::int64_t> recorded_samples_{0};
    std::atomic<bool> device_running_{false};
    std::atomic<int> latency_samples_{0};
    std::unique_ptr<juce::TimeSliceThread> writer_thread_;

    // Automation (message thread unless noted)
    std::map<std::string, std::shared_ptr<AutoSlot>> auto_slots_;  // owner + '\x1f' + param
    std::vector<WriteSegment> segments_;                            // open in this pass
    std::vector<session::Session::AutomationWrite> written_;       // closed, committed on stop
    std::set<std::string> touched_;                                 // keys of controls held now
    bool writing_pass_ = false;
    bool committing_ = false;
    std::shared_ptr<GestureQueue> gestures_ = std::make_shared<GestureQueue>();
    int next_watch_ = 1;
    std::map<const void*, int> watch_ids_;
    std::map<int, std::pair<std::string, std::string>> insert_watch_targets_, instrument_watch_targets_;
    /// Each track's MIDI, kept between rebuilds and re-rendered only where
    /// an edit reaches (Main Spec §8). Message thread only.
    project::RenderCache render_cache_;
    // Delay compensation (message thread)
    std::map<std::string, std::shared_ptr<DelayLine>> delay_lines_;
    std::map<std::string, int> strip_latency_;
    int compensation_ = 0;
    std::vector<int> latencies_seen_;
    // What is heard: the render position minus the compensation (audio thread writes).
    std::atomic<std::int64_t> heard_samples_{0};
    std::int64_t play_start_ = 0;  // audio thread
    bool rolling_ = false;         // audio thread: playing at the last block

    juce::MidiMessageCollector live_input_;
    std::atomic<int> live_octave_{0};
    std::array<std::atomic<int>, 128> live_held_{};
    std::string live_track_;
    void refresh_live_input();
    juce::CriticalSection live_input_lock_;
    juce::String live_input_device_;
    std::atomic<int> live_track_index_{-1};
    SurfaceInput surface_input_;
    SurfaceInput mixer_surface_input_;
    std::atomic<bool> learn_armed_{false};
    std::atomic<std::uint64_t> input_filter_{pack(InputFilter{})};
    audio::SpscQueue<LiveEvent> live_log_{4096};
    std::deque<LiveEvent> live_history_;
    void drain_live_log();
    std::atomic<bool> monitor_dim_{false}, monitor_mono_{false}, monitor_mute_{false};
    std::atomic<float> monitor_dim_gain_{0.1f};
    std::atomic<int> monitor_pair_{0};
    double monitor_dim_db_ = -20.0;
    std::atomic<std::uint32_t> learned_{0};
    // Varispeed: the mix rendered into this at rate x the block, then resampled.
    void process_varispeed(float* const* outputs, int num_outputs, int num_samples) noexcept;
    std::atomic<double> varispeed_{1.0};
    juce::AudioBuffer<float> vs_buffer_;
    std::array<juce::LagrangeInterpolator, 2> vs_interp_;
    double vs_frac_ = 0.0;
    // Scrub grains (message thread).
    bool grain_active_ = false;
    Ticks grain_return_ = 0;
    double grain_until_ms_ = 0.0;
    std::unique_ptr<juce::MidiInputCallback> midi_callback_;
    bool auto_enable_midi_ = true;
    juce::StringArray known_midi_;
    std::unique_ptr<juce::ChangeListener> device_listener_;
    juce::MidiDeviceListConnection midi_list_;
};

}  // namespace vcc::desktop
