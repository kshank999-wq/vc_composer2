// Plug-in hosting (Main Spec §10, §12): the known-plug-in list, scanning with
// crash protection, creating instrument instances from a saved PluginRef, and
// moving a plug-in's state in and out of the project.
//
// Scanning is out of process (Main Spec §20 "a failed scan cannot crash the
// whole project"): every plug-in file is loaded by a separate
// vcc_plugin_scanner process with a time limit. A plug-in that crashes,
// hangs or reports garbage only takes that process down; it is blacklisted
// with the reason, and the scan carries on. If the scanner tool is missing
// (a broken install), scanning falls back to in-process with JUCE's
// dead-man's-pedal file, which at least skips a crasher on the next launch,
// and says so.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

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

    // --- Out-of-process scanning ---------------------------------------------------
    /// The scanner tool beside the application, or the one set here (the
    /// checks point at their build's). An empty/missing file means scanning
    /// runs in process.
    juce::File scanner_tool() const;
    void set_scanner_tool(const juce::File& tool);
    bool scans_out_of_process() const { return scanner_tool().existsAsFile(); }
    /// How long one plug-in file may take to scan before it is stopped.
    void set_scan_timeout_ms(int ms) noexcept { scan_timeout_ms_.store(ms); }
    int scan_timeout_ms() const noexcept { return scan_timeout_ms_.load(); }
    /// Scans one file or identifier through the scanner tool (any thread).
    /// Returns false - and records why - when the plug-in crashed, hung or
    /// could not be read; `found` gets what it contains otherwise.
    bool scan_file(juce::AudioPluginFormat& format, const juce::String& file_or_id,
                   juce::OwnedArray<juce::PluginDescription>& found, const std::function<bool()>& cancelled = {});
    /// Why plug-ins failed to scan: file or identifier -> reason. Saved with
    /// the plug-in list.
    std::map<juce::String, juce::String> scan_failures() const;
    void clear_scan_failure(const juce::String& file_or_id);

    /// Instruments in the known list, sorted by vendor and name.
    juce::Array<juce::PluginDescription> instruments() const;

    project::PluginRef ref_for(const juce::PluginDescription& description) const;

    // --- Hosting modes --------------------------------------------------------------
    /// Where plug-ins run. Out of the app, a crashing plug-in takes down only
    /// its host process (and whatever else that process carries):
    ///   in_app           inside VC Composer (lightest; a crash takes the app down)
    ///   each_instance    every instance in its own process (safest; the default)
    ///   per_plugin       all instances of one plug-in share a process
    ///   per_manufacturer all of one manufacturer's plug-ins share a process
    ///   shared           every plug-in in one process (lightest out of the app)
    enum class HostingMode { in_app, each_instance, per_plugin, per_manufacturer, shared };
    static juce::StringArray hosting_mode_names();
    HostingMode hosting_mode() const;
    void set_hosting_mode(HostingMode mode);
    juce::File host_tool() const;
    void set_host_tool(const juce::File& tool);
    bool hosts_out_of_process() const { return hosting_mode() != HostingMode::in_app && host_tool().existsAsFile(); }
    /// A plug-in whose process crashed this session; it runs in its own
    /// process from its next load, whatever the mode (in a shared process
    /// there is no telling which plug-in crashed it, so all of them are).
    void note_crash(const juce::String& plugin_identifier);
    bool isolated(const juce::String& plugin_identifier) const;
    /// Lets plug-ins that crashed share processes again (the mode applies).
    void clear_crash_isolation();
    /// Host processes running now, and the instances each carries (for the
    /// Plug-ins window and the checks).
    std::vector<std::pair<juce::String, int>> host_processes() const;

    /// Creates and prepares a plug-in instance for a saved reference,
    /// restoring its state. Returns nullptr with a reason when the plug-in is
    /// not installed on this machine (Main Spec §15: show it missing, keep
    /// the track, allow a substitute).
    std::unique_ptr<juce::AudioPluginInstance> create(const project::PluginRef& ref, double sample_rate,
                                                      int block_size, juce::String& error);

    static std::string state_of(juce::AudioPluginInstance& instance);
    static void restore_state(juce::AudioPluginInstance& instance, const std::string& state_base64);

private:
    void record_failure(const juce::String& file_or_id, const juce::String& reason);

    juce::AudioPluginFormatManager formats_;
    juce::KnownPluginList known_;
    std::unique_ptr<juce::PropertiesFile> properties_;
    mutable std::mutex scan_lock_;
    juce::File scanner_override_;
    juce::File host_override_;
    std::shared_ptr<class HostProcess> process_for(const juce::PluginDescription& description);
    mutable std::mutex hosting_lock_;
    std::map<juce::String, std::weak_ptr<class HostProcess>> processes_;
    std::vector<std::weak_ptr<class HostProcess>> all_processes_;
    std::set<juce::String> isolated_;
    std::atomic<int> scan_timeout_ms_{60000};
    std::map<juce::String, juce::String> failures_;
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
