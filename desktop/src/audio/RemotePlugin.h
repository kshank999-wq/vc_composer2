// A plug-in running in its own process (Main Spec §20), seen by the app as an
// ordinary juce::AudioPluginInstance - instruments, effect inserts, state,
// parameter automation, latency and editor windows all work unchanged.
//
// Audio: processBlock hands the block, its MIDI and any automation to the
// plug-in's process through shared memory and waits for it. On the device's
// audio thread the wait ends with the block's deadline: a late plug-in plays
// silence for that block (counted) rather than stalling the whole mix. An
// offline render (on the message thread, or a non-realtime instance) waits
// for every block, so bounces and stems are exact.
//
// If the plug-in's process dies, the instance goes silent and says it
// crashed; the rest of the session plays on. Its last known state is kept so
// the project never loses it, and PlaybackEngine::reload_crashed_plugins()
// starts a fresh process from it.
#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

#include "audio/PluginIpc.h"

namespace vcc::desktop {

class RemotePluginInstance final : public juce::AudioPluginInstance {
public:
    /// Starts `host_tool`, loads `description` with `state_base64` into it.
    /// Null with `error` set when the process could not start or the plug-in
    /// failed to load there.
    static std::unique_ptr<RemotePluginInstance> create(const juce::File& host_tool,
                                                        const juce::PluginDescription& description,
                                                        const std::string& state_base64, double sample_rate,
                                                        int block_size, juce::String& error);
    ~RemotePluginInstance() override;

    /// The plug-in's process has died (crashed, or was killed for hanging).
    bool crashed() const noexcept { return crashed_.load(); }
    /// Blocks played as silence because the plug-in was late, since the last call.
    int take_late_blocks() noexcept { return late_.exchange(0); }

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

private:
    class Connection;
    class Parameter;
    struct Pending;

    RemotePluginInstance(const BusesProperties& buses, juce::PluginDescription description);
    /// Sends a control message and waits for its reply (message thread).
    bool call(const juce::String& command, const juce::MemoryBlock& payload, juce::MemoryBlock& reply,
              int timeout_ms);
    void received(const juce::MemoryBlock& block);
    void lost();
    void run_block(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, bool bypassed) noexcept;
    void push_param(int index, float value) noexcept;

    juce::PluginDescription description_;
    std::unique_ptr<Connection> connection_;
    ipc::SharedBlock shared_;
    juce::String name_;
    int ins_ = 0, outs_ = 2;
    bool accepts_midi_ = true;
    double tail_ = 0.0;
    double sample_rate_ = 48000.0;
    std::atomic<bool> crashed_{false};
    std::atomic<int> late_{0};
    std::atomic<bool> waiting_{false};  ///< A block is out: the next must wait for it.
    std::uint32_t sent_ = 0;
    std::string last_state_;            ///< base64; what is saved if the process is gone
    juce::SpinLock param_lock_;         ///< Writers to the to-plug-in ring (audio + message thread).
    std::vector<Parameter*> params_;

    std::mutex pending_lock_;
    std::map<int, std::shared_ptr<Pending>> pending_;
    std::atomic<int> next_id_{1};
};

}  // namespace vcc::desktop
