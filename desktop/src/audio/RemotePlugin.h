// Plug-ins running outside the app (Main Spec §20), seen by the app as
// ordinary juce::AudioPluginInstances - instruments, effect inserts, state,
// parameter automation, latency and editor windows all work unchanged.
//
// A HostProcess is one vcc_plugin_host process; it can carry any number of
// instances (the hosting mode decides how many: one, all of one plug-in,
// all of one manufacturer, or everything). Each RemotePluginInstance keeps
// its process alive; the process stops when its last instance goes.
//
// Audio: processBlock hands the block, its MIDI and any automation to the
// plug-in through shared memory and waits for it. On the device's audio
// thread the wait ends with the block's deadline: a late plug-in plays
// silence for that block (counted) rather than stalling the whole mix. An
// offline render (on the message thread, or a non-realtime instance) waits
// for every block, so bounces and stems are exact.
//
// If a process dies, every instance in it goes silent and says it crashed;
// the rest of the session plays on. Each keeps its last known state so the
// project never loses it, and PlaybackEngine::reload_crashed_plugins()
// starts them again from it.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "audio/PluginIpc.h"

namespace vcc::desktop {

/// One plug-in host process and its connection.
class HostProcess final {
public:
    /// Starts the tool. Null when it could not be started.
    static std::shared_ptr<HostProcess> start(const juce::File& host_tool, const juce::String& label);
    ~HostProcess();

    bool crashed() const noexcept { return crashed_.load(); }
    /// Why it is marked crashed: the connection dropped (the process exited
    /// or died) or it stopped answering. Empty while it runs.
    juce::String crash_reason() const
    {
        std::lock_guard<std::mutex> lock(pending_lock_);
        return reason_;
    }
    /// What the process is for ("VCC Test Effect", "Spitfire Audio", "all plug-ins").
    const juce::String& label() const noexcept { return label_; }
    int new_instance_id() noexcept { return next_instance_++; }
    /// Instances it carries now.
    int instance_count() const noexcept { return instances_.load(); }

    /// Sends a control message for an instance and waits for its reply.
    bool call(int instance, const juce::String& command, const juce::MemoryBlock& payload, juce::MemoryBlock& reply,
              int timeout_ms);
    /// Its physical memory, as it reports it (a short control call; 0 when
    /// it does not answer). Call off the audio thread.
    std::uint64_t memory_bytes(int timeout_ms = 500);
    /// Marks the process dead (an instance found it hung) and stops it.
    void mark_crashed();

private:
    friend class RemotePluginInstance;
    class Connection;
    struct Pending;
    HostProcess() = default;
    void received(const juce::MemoryBlock& block);
    void lost();

    std::unique_ptr<Connection> connection_;
    juce::String label_;
    std::atomic<bool> crashed_{false};
    std::atomic<int> next_instance_{1};
    std::atomic<int> instances_{0};
    mutable std::mutex pending_lock_;
    juce::String reason_;  // guarded by pending_lock_
    std::map<int, std::shared_ptr<Pending>> pending_;
    std::atomic<int> next_id_{1};
};

class RemotePluginInstance final : public juce::AudioPluginInstance, private juce::Timer {
public:
    /// Loads `description` with `state_base64` into `process`. Null with
    /// `error` set when the plug-in failed to load there.
    static std::unique_ptr<RemotePluginInstance> create(std::shared_ptr<HostProcess> process,
                                                        const juce::PluginDescription& description,
                                                        const std::string& state_base64, double sample_rate,
                                                        int block_size, juce::String& error);
    ~RemotePluginInstance() override;

    /// The plug-in's process has died (crashed, or was stopped for hanging).
    bool crashed() const noexcept { return process_->crashed(); }
    /// Blocks played as silence because the plug-in was late, since the last call.
    int take_late_blocks() noexcept { return late_.exchange(0); }
    /// The process it runs in (shared with other instances in shared modes).
    const HostProcess& process() const noexcept { return *process_; }

    // juce::AudioPluginInstance
    const juce::String getName() const override;
    void fillInPluginDescription(juce::PluginDescription& d) const override { d = description_; }
    void prepareToPlay(double sample_rate, int block_size) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using juce::AudioPluginInstance::processBlock;
    double getTailLengthSeconds() const override { return tail_; }
    bool acceptsMidi() const override { return accepts_midi_; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& data) override;
    void setStateInformation(const void* data, int size) override;

    /// Shows or hides the plug-in's own window (in its process).
    bool show_editor(bool show);
    /// The plug-in window's title ("Violins I - Opus"), as a DAW names it.
    void set_editor_title(const juce::String& title) { editor_title_ = title; }
    /// Whether its window was last asked to show, and did.
    bool editor_shown() const noexcept { return editor_shown_; }

private:
    class Parameter;

    RemotePluginInstance(const BusesProperties& buses, juce::PluginDescription description,
                         std::shared_ptr<HostProcess> process);
    bool call(const juce::String& command, const juce::MemoryBlock& payload, juce::MemoryBlock& reply, int timeout_ms)
    {
        return process_->call(instance_, command, payload, reply, timeout_ms);
    }
    void run_block(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, bool bypassed) noexcept;
    /// Picks up the plug-in's latency (message thread; it can change
    /// whether or not audio is running).
    void timerCallback() override;
    void push_param(int index, float value) noexcept;

    juce::PluginDescription description_;
    std::shared_ptr<HostProcess> process_;
    int instance_ = 0;
    bool loaded_ = false;
    ipc::SharedBlock shared_;
    juce::String name_;
    int ins_ = 0, outs_ = 2;
    bool accepts_midi_ = true;
    double tail_ = 0.0;
    double sample_rate_ = 48000.0;
    std::atomic<int> late_{0};
    std::uint32_t sent_ = 0;
    std::string last_state_;     ///< base64; what is saved if the process is gone
    juce::SpinLock param_lock_;  ///< Writers to the to-plug-in ring (audio + message thread).
    std::vector<Parameter*> params_;
    bool editor_shown_ = false;
    juce::String editor_title_;
};

}  // namespace vcc::desktop
