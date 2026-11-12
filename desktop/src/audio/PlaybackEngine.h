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
#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "audio/Instrument.h"
#include "audio/PluginHost.h"
#include "vcc/audio/SpscRingBuffer.h"
#include "vcc/core/Midi.h"
#include "vcc/session/Session.h"

namespace vcc::desktop {

struct PlaybackSnapshot;
struct PreviewClip;

class PlaybackEngine final : public juce::AudioIODeviceCallback, private juce::Timer {
public:
    PlaybackEngine(session::Session& session, PluginHost& plugins);
    ~PlaybackEngine() override;

    /// Opens the default output device. Returns an empty string, or why there
    /// is no audio (the app keeps working silently).
    juce::String open_default_device();
    juce::AudioDeviceManager& devices() noexcept { return devices_; }

    /// Prepares for offline rendering without a device.
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
    void set_metronome(bool on) noexcept { metronome_ = on; }
    bool metronome() const noexcept { return metronome_.load(); }

    // --- Audition (Chord Explorer, Ostinato Creator, template preview) -----------
    /// Plays notes through a placeholder voice, independent of the transport
    /// and the project: nothing is written (Supplement §9.1, §10.2, T05).
    void preview(const std::vector<NoteEvent>& notes, const std::string& family, int gm_program, double bpm,
                 bool loop, Ticks loop_length = 0);
    void stop_preview();
    bool previewing() const noexcept { return previewing_.load(); }

    // --- Instruments and meters ---------------------------------------------------
    /// What is playing a track: "Placeholder (bowed)", a plug-in name, or why
    /// a saved plug-in could not be loaded.
    juce::String instrument_status(const std::string& track_id) const;
    juce::AudioPluginInstance* plugin_for(const std::string& track_id) const;
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
    bool punch() const noexcept;
    /// Seconds recorded in the current pass.
    double record_seconds() const noexcept;
    /// Called on the message thread after a pass is placed (one undo step),
    /// or with an error ("disk full", "no input device").
    std::function<void(const juce::String& message, bool ok)> on_recording_finished;
    /// Offline rendering with a given input (tests and the headless check):
    /// `input` stands in for the device's input channels.
    void render_offline_with_input(const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& out,
                                   int num_samples);

    /// Re-render the snapshot now (normally debounced after document changes).
    void rebuild_now();

    /// Live MIDI input is played on this track's instrument.
    void set_live_track(const std::string& track_id);

    std::function<void()> on_transport_changed;

    // juce::AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext(const float* const* inputs, int num_inputs, float* const* outputs,
                                          int num_outputs, int num_samples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    struct InstrumentSlot {
        std::string key;  ///< what it was built from; rebuilt when this changes
        std::shared_ptr<Instrument> instrument;
        juce::String status;
    };

    void timerCallback() override;
    void request_rebuild();
    void prepare_all(double sample_rate, int block_size);
    std::shared_ptr<Instrument> instrument_for(const project::Track& track, InstrumentSlot& slot);
    std::unique_ptr<PlaybackSnapshot> build_snapshot();
    void process(float* const* outputs, int num_outputs, int num_samples) noexcept;
    void process_chunk(float* const* outputs, int num_outputs, int offset, int num_samples) noexcept;
    void retire_garbage();

    session::Session& session_;
    PluginHost& plugins_;
    juce::AudioDeviceManager devices_;
    int listener_ = 0;

    double sample_rate_ = 48000.0;
    int block_size_ = 512;

    std::map<std::string, InstrumentSlot> instruments_;  // message thread only
    std::map<std::string, int> meter_index_;             // track id -> meter slot
    std::array<std::atomic<float>, 512> meters_{};
    std::atomic<float> master_meter_{0.0f};

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
    std::atomic<std::int64_t> locate_request_{-1};
    std::atomic<std::int64_t> position_samples_{0};
    std::atomic<bool> metronome_{false};
    std::atomic<bool> previewing_{false};
    bool rebuild_pending_ = false;

    juce::MidiMessageCollector live_input_;
    std::string live_track_;
    std::atomic<int> live_track_index_{-1};
    std::unique_ptr<juce::MidiInputCallback> midi_callback_;
};

}  // namespace vcc::desktop
