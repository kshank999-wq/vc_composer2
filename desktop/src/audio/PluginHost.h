// Plug-in hosting (Main Spec §10, §12): the known-plug-in list, scanning with
// crash protection, creating instrument instances from a saved PluginRef, and
// moving a plug-in's state in and out of the project.
//
// Scanning uses JUCE's dead-man's-pedal file: a plug-in that crashes the
// scan is recorded and skipped next time, so one bad plug-in cannot keep the
// application from starting (Main Spec §20 "failed scan cannot crash the
// whole project" - the remaining step is out-of-process scanning).
#pragma once

#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

#include "audio/AutomationRt.h"
#include "audio/Instrument.h"
#include "vcc/project/Project.h"

namespace vcc::desktop {

class PluginHost {
public:
    PluginHost();
    ~PluginHost();

    juce::AudioPluginFormatManager& formats() noexcept { return formats_; }
    juce::KnownPluginList& known() noexcept { return known_; }
    juce::File dead_mans_pedal() const;
    juce::PropertiesFile* properties() noexcept { return properties_.get(); }

    /// Saves the known-plug-in list (call after a scan).
    void save_list();

    /// Instruments in the known list, sorted by vendor and name.
    juce::Array<juce::PluginDescription> instruments() const;

    project::PluginRef ref_for(const juce::PluginDescription& description) const;

    /// Creates and prepares a plug-in instance for a saved reference,
    /// restoring its state. Returns nullptr with a reason when the plug-in is
    /// not installed on this machine (Main Spec §15: show it missing, keep
    /// the track, allow a substitute).
    std::unique_ptr<juce::AudioPluginInstance> create(const project::PluginRef& ref, double sample_rate,
                                                      int block_size, juce::String& error);

    static std::string state_of(juce::AudioPluginInstance& instance);
    static void restore_state(juce::AudioPluginInstance& instance, const std::string& state_base64);

private:
    juce::AudioPluginFormatManager formats_;
    juce::KnownPluginList known_;
    std::unique_ptr<juce::PropertiesFile> properties_;
};

/// A hosted plug-in as a track instrument.
class PluginInstrument final : public Instrument {
public:
    explicit PluginInstrument(std::unique_ptr<juce::AudioPluginInstance> instance);
    ~PluginInstrument() override;

    void prepare(double sample_rate, int max_block_size) override;
    void process(juce::AudioBuffer<float>& out, juce::MidiBuffer& midi, int num_samples) noexcept override;
    void all_notes_off() noexcept override;
    bool wants_keyswitches() const noexcept override { return true; }
    juce::String display_name() const override;
    int latency_samples() const noexcept override { return instance_->getLatencySamples(); }

    juce::AudioPluginInstance& instance() noexcept { return *instance_; }
    /// Listens to the plug-in's parameter gestures (message thread).
    void set_watch(std::unique_ptr<ParameterWatch> watch) { watch_ = std::move(watch); }
    bool has_watch() const noexcept { return watch_ != nullptr; }

private:
    std::unique_ptr<juce::AudioPluginInstance> instance_;
    std::unique_ptr<ParameterWatch> watch_;  // destroyed before the instance
    juce::AudioBuffer<float> buffer_;
    juce::MidiBuffer panic_;
    std::atomic<bool> panic_pending_{false};
};

}  // namespace vcc::desktop
